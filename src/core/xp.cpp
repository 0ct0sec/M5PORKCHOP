// Porkchop RPG XP and Leveling System Implementation

#include "xp.h"
#include "../ui/display.h"

// Static member initialization
PorkXPData XP::data = {0};
SessionStats XP::session = {0};
Preferences XP::prefs;
bool XP::initialized = false;
void (*XP::levelUpCallback)(uint8_t, uint8_t) = nullptr;

// XP values for each event type (synced with docs/RPG_IMPLEMENTATION_PLAN.txt)
static const uint16_t XP_VALUES[] = {
    1,      // NETWORK_FOUND
    3,      // NETWORK_HIDDEN (was 5, doc says 3)
    10,     // NETWORK_WPA3
    3,      // NETWORK_OPEN
    50,     // HANDSHAKE_CAPTURED
    75,     // PMKID_CAPTURED
    2,      // DEAUTH_SENT
    15,     // DEAUTH_SUCCESS
    2,      // WARHOG_LOGGED
    25,     // DISTANCE_KM
    2,      // BLE_BURST (was 1, doc says 2)
    3,      // BLE_APPLE
    5,      // GPS_LOCK (was 10, doc says 5)
    25,     // ML_ROGUE_DETECTED
    10,     // SESSION_30MIN (was 50, doc says 10)
    25,     // SESSION_60MIN (was 100, doc says 25)
    50,     // SESSION_120MIN (was 200, doc says 50)
    20      // LOW_BATTERY_CAPTURE
};

// 40 rank titles - hacker/grindhouse/tarantino + pig flavor
static const char* RANK_TITLES[] = {
    // Tier 1: Noob (1-5)
    "BACON N00B",
    "SCRIPT PIGG0",
    "PIGLET 0DAY",
    "SNOUT SCAN",
    "SLOP NMAP",
    // Tier 2: Beginner (6-10)
    "BEACON BOAR",
    "CHAN H4M",
    "PROBE PORK",
    "SSID SW1NE",
    "PKT PIGLET",
    // Tier 3: Intermediate (11-15)
    "DEAUTH H0G",
    "HANDSHAKE HAM",
    "PMKID PORK",
    "EAPOL B0AR",
    "SAUSAGE SYNC",
    // Tier 4: Skilled (16-20)
    "WARDRIVE HOG",
    "GPS L0CK PIG",
    "BLE SPAM HAM",
    "TRUFFLE R00T",
    "INJECT P1G",
    // Tier 5: Advanced (21-25)
    "KARMA SW1NE",
    "EVIL TWIN H0G",
    "KERNEL BAC0N",
    "MON1TOR BOAR",
    "WPA3 WARTH0G",
    // Tier 6: Expert (26-30)
    "KRACK SW1NE",
    "FR4G ATTACK",
    "DRAGONBL00D",
    "DEATH PR00F",
    "PLANET ERR0R",
    // Tier 7: Elite (31-35)
    "P0RK FICTION",
    "RESERVOIR H0G",
    "HATEFUL 0INK",
    "JACK1E B0AR",
    "80211 WARL0RD",
    // Tier 8: Legendary (36-40)
    "MACHETE SW1NE",
    "CRUNCH P1G",
    "DARK TANGENT",
    "PHIBER 0PT1K",
    "MUDGE UNCHA1NED"
};
static const uint8_t MAX_LEVEL = 40;

// Achievement names
static const char* ACHIEVEMENT_NAMES[] = {
    "FIRST BLOOD",
    "CENTURION",
    "MARATHON PIG",
    "NIGHT OWL",
    "GHOST HUNTER",
    "APPLE FARMER",
    "WARDRIVER",
    "DEAUTH KING",
    "PMKID HUNTER",
    "WPA3 SPOTTER",
    "GPS MASTER",
    "TOUCH GRASS",
    "SILICON PSYCHO",
    "CLUTCH CAPTURE",
    "SPEED RUN",
    "CHAOS AGENT"
};

// Level up phrases
static const char* LEVELUP_PHRASES[] = {
    "snout grew stronger",
    "new truffle unlocked",
    "skill issue? not anymore",
    "gg ez level up",
    "evolution complete",
    "power level rising",
    "oink intensifies",
    "XP printer go brrr",
    "grinding them levels",
    "swine on the rise"
};
static const uint8_t LEVELUP_PHRASE_COUNT = sizeof(LEVELUP_PHRASES) / sizeof(LEVELUP_PHRASES[0]);

void XP::init() {
    if (initialized) return;
    
    load();
    startSession();
    initialized = true;
    
    Serial.printf("[XP] Initialized - LV%d %s (%lu XP)\n", 
                  getLevel(), getTitle(), data.totalXP);
}

void XP::load() {
    prefs.begin("porkxp", true);  // Read-only
    
    data.totalXP = prefs.getUInt("totalxp", 0);
    data.achievements = prefs.getUInt("achieve", 0);
    data.lifetimeNetworks = prefs.getUInt("networks", 0);
    data.lifetimeHS = prefs.getUInt("hs", 0);
    data.lifetimeDeauths = prefs.getUInt("deauths", 0);
    data.lifetimeDistance = prefs.getUInt("distance", 0);
    data.lifetimeBLE = prefs.getUInt("ble", 0);
    data.hiddenNetworks = prefs.getUInt("hidden", 0);
    data.wpa3Networks = prefs.getUInt("wpa3", 0);
    data.gpsNetworks = prefs.getUInt("gpsnet", 0);
    data.sessions = prefs.getUShort("sessions", 0);
    data.cachedLevel = calculateLevel(data.totalXP);
    
    prefs.end();
}

void XP::save() {
    prefs.begin("porkxp", false);  // Read-write
    
    prefs.putUInt("totalxp", data.totalXP);
    prefs.putUInt("achieve", data.achievements);
    prefs.putUInt("networks", data.lifetimeNetworks);
    prefs.putUInt("hs", data.lifetimeHS);
    prefs.putUInt("deauths", data.lifetimeDeauths);
    prefs.putUInt("distance", data.lifetimeDistance);
    prefs.putUInt("ble", data.lifetimeBLE);
    prefs.putUInt("hidden", data.hiddenNetworks);
    prefs.putUInt("wpa3", data.wpa3Networks);
    prefs.putUInt("gpsnet", data.gpsNetworks);
    prefs.putUShort("sessions", data.sessions);
    
    prefs.end();
    
    Serial.printf("[XP] Saved - LV%d (%lu XP)\n", getLevel(), data.totalXP);
}

// Static for km tracking - needs to be reset on session start
static uint32_t lastKmAwarded = 0;

void XP::startSession() {
    memset(&session, 0, sizeof(session));
    session.startTime = millis();
    lastKmAwarded = 0;  // Reset km counter for new session
    data.sessions++;
}

void XP::endSession() {
    save();
    Serial.printf("[XP] Session ended - +%lu XP this session\n", session.xp);
}

void XP::addXP(XPEvent event) {
    uint16_t amount = XP_VALUES[static_cast<uint8_t>(event)];
    
    // Track lifetime stats based on event type
    switch (event) {
        case XPEvent::NETWORK_FOUND:
        case XPEvent::NETWORK_OPEN:
            data.lifetimeNetworks++;
            session.networks++;
            break;
        case XPEvent::NETWORK_HIDDEN:
            data.lifetimeNetworks++;
            data.hiddenNetworks++;
            session.networks++;
            break;
        case XPEvent::NETWORK_WPA3:
            data.lifetimeNetworks++;
            data.wpa3Networks++;
            session.networks++;
            break;
        case XPEvent::HANDSHAKE_CAPTURED:
        case XPEvent::PMKID_CAPTURED:
            data.lifetimeHS++;
            session.handshakes++;
            break;
        case XPEvent::DEAUTH_SUCCESS:
            data.lifetimeDeauths++;
            session.deauths++;
            break;
        case XPEvent::DEAUTH_SENT:
            // Don't count sent, only success
            break;
        case XPEvent::WARHOG_LOGGED:
            data.gpsNetworks++;
            break;
        case XPEvent::BLE_BURST:
        case XPEvent::BLE_APPLE:
            data.lifetimeBLE++;
            session.blePackets++;
            break;
        case XPEvent::GPS_LOCK:
            session.gpsLockAwarded = true;
            break;
        default:
            break;
    }
    
    addXP(amount);
    checkAchievements();
}

void XP::addXP(uint16_t amount) {
    uint8_t oldLevel = data.cachedLevel;
    
    data.totalXP += amount;
    session.xp += amount;
    
    uint8_t newLevel = calculateLevel(data.totalXP);
    
    if (newLevel > oldLevel) {
        data.cachedLevel = newLevel;
        Serial.printf("[XP] LEVEL UP! %d -> %d (%s)\n", 
                      oldLevel, newLevel, getTitleForLevel(newLevel));
        
        if (levelUpCallback) {
            levelUpCallback(oldLevel, newLevel);
        }
    }
}

void XP::addDistance(uint32_t meters) {
    data.lifetimeDistance += meters;
    session.distanceM += meters;
    
    // Award XP per km (check if we crossed a km boundary)
    // lastKmAwarded is defined at file scope and reset in startSession()
    uint32_t currentKm = session.distanceM / 1000;
    
    if (currentKm > lastKmAwarded) {
        uint32_t newKms = currentKm - lastKmAwarded;
        for (uint32_t i = 0; i < newKms; i++) {
            addXP(XPEvent::DISTANCE_KM);
        }
        lastKmAwarded = currentKm;
    }
}

void XP::updateSessionTime() {
    uint32_t sessionMinutes = (millis() - session.startTime) / 60000;
    
    if (sessionMinutes >= 30 && !session.session30Awarded) {
        addXP(XPEvent::SESSION_30MIN);
        session.session30Awarded = true;
    }
    if (sessionMinutes >= 60 && !session.session60Awarded) {
        addXP(XPEvent::SESSION_60MIN);
        session.session60Awarded = true;
    }
    if (sessionMinutes >= 120 && !session.session120Awarded) {
        addXP(XPEvent::SESSION_120MIN);
        session.session120Awarded = true;
    }
}

uint8_t XP::calculateLevel(uint32_t xp) {
    // XP thresholds for each level
    // Designed for: L1-5 quick, L6-20 steady, L21-40 grind
    uint32_t thresholds[MAX_LEVEL] = {
        0,      // L1
        100,    // L2
        300,    // L3
        600,    // L4
        1000,   // L5
        1500,   // L6
        2300,   // L7
        3400,   // L8
        4800,   // L9
        6500,   // L10
        8500,   // L11
        11000,  // L12
        14000,  // L13
        17500,  // L14
        21500,  // L15
        26000,  // L16
        31000,  // L17
        36500,  // L18
        42500,  // L19
        49000,  // L20
        56000,  // L21
        64000,  // L22
        73000,  // L23
        83000,  // L24
        94000,  // L25
        106000, // L26
        120000, // L27
        136000, // L28
        154000, // L29
        174000, // L30
        197000, // L31
        223000, // L32
        252000, // L33
        284000, // L34
        319000, // L35
        359000, // L36
        404000, // L37
        454000, // L38
        514000, // L39
        600000  // L40
    };
    
    for (uint8_t i = MAX_LEVEL - 1; i > 0; i--) {
        if (xp >= thresholds[i]) {
            return i + 1;  // Levels are 1-indexed
        }
    }
    return 1;
}

uint32_t XP::getXPForLevel(uint8_t level) {
    if (level <= 1) return 0;
    if (level > MAX_LEVEL) level = MAX_LEVEL;
    
    // Same thresholds as calculateLevel
    uint32_t thresholds[MAX_LEVEL] = {
        0, 100, 300, 600, 1000, 1500, 2300, 3400, 4800, 6500,
        8500, 11000, 14000, 17500, 21500, 26000, 31000, 36500, 42500, 49000,
        56000, 64000, 73000, 83000, 94000, 106000, 120000, 136000, 154000, 174000,
        197000, 223000, 252000, 284000, 319000, 359000, 404000, 454000, 514000, 600000
    };
    
    return thresholds[level - 1];
}

uint8_t XP::getLevel() {
    return data.cachedLevel > 0 ? data.cachedLevel : 1;
}

uint32_t XP::getTotalXP() {
    return data.totalXP;
}

uint32_t XP::getXPToNextLevel() {
    uint8_t level = getLevel();
    if (level >= MAX_LEVEL) return 0;
    
    return getXPForLevel(level + 1) - data.totalXP;
}

uint8_t XP::getProgress() {
    uint8_t level = getLevel();
    if (level >= MAX_LEVEL) return 100;
    
    uint32_t currentLevelXP = getXPForLevel(level);
    uint32_t nextLevelXP = getXPForLevel(level + 1);
    uint32_t levelRange = nextLevelXP - currentLevelXP;
    uint32_t progress = data.totalXP - currentLevelXP;
    
    if (levelRange == 0) return 100;
    return (progress * 100) / levelRange;
}

const char* XP::getTitle() {
    return getTitleForLevel(getLevel());
}

const char* XP::getTitleForLevel(uint8_t level) {
    if (level < 1) level = 1;
    if (level > MAX_LEVEL) level = MAX_LEVEL;
    return RANK_TITLES[level - 1];
}

void XP::unlockAchievement(PorkAchievement ach) {
    if (hasAchievement(ach)) return;
    
    data.achievements |= ach;
    
    // Find achievement index for name lookup
    uint8_t idx = 0;
    uint32_t mask = 1;
    while (mask < (uint32_t)ach && idx < 16) {
        mask <<= 1;
        idx++;
    }
    
    Serial.printf("[XP] Achievement unlocked: %s\n", ACHIEVEMENT_NAMES[idx]);
    
    // Could trigger a toast here
}

bool XP::hasAchievement(PorkAchievement ach) {
    return (data.achievements & ach) != 0;
}

uint32_t XP::getAchievements() {
    return data.achievements;
}

const char* XP::getAchievementName(PorkAchievement ach) {
    uint8_t idx = 0;
    uint32_t mask = 1;
    while (mask < (uint32_t)ach && idx < 16) {
        mask <<= 1;
        idx++;
    }
    return ACHIEVEMENT_NAMES[idx];
}

void XP::checkAchievements() {
    // First handshake
    if (data.lifetimeHS >= 1 && !hasAchievement(ACH_FIRST_BLOOD)) {
        unlockAchievement(ACH_FIRST_BLOOD);
    }
    
    // 100 networks in session
    if (session.networks >= 100 && !hasAchievement(ACH_CENTURION)) {
        unlockAchievement(ACH_CENTURION);
    }
    
    // 10km walked (session)
    if (session.distanceM >= 10000 && !hasAchievement(ACH_MARATHON_PIG)) {
        unlockAchievement(ACH_MARATHON_PIG);
    }
    
    // 10 hidden networks
    if (data.hiddenNetworks >= 10 && !hasAchievement(ACH_GHOST_HUNTER)) {
        unlockAchievement(ACH_GHOST_HUNTER);
    }
    
    // 100 Apple BLE hits (check lifetimeBLE, rough proxy)
    if (data.lifetimeBLE >= 100 && !hasAchievement(ACH_APPLE_FARMER)) {
        unlockAchievement(ACH_APPLE_FARMER);
    }
    
    // 1000 lifetime networks
    if (data.lifetimeNetworks >= 1000 && !hasAchievement(ACH_WARDRIVER)) {
        unlockAchievement(ACH_WARDRIVER);
    }
    
    // 100 successful deauths
    if (data.lifetimeDeauths >= 100 && !hasAchievement(ACH_DEAUTH_KING)) {
        unlockAchievement(ACH_DEAUTH_KING);
    }
    
    // WPA3 network found
    if (data.wpa3Networks >= 1 && !hasAchievement(ACH_WPA3_SPOTTER)) {
        unlockAchievement(ACH_WPA3_SPOTTER);
    }
    
    // 100 GPS-tagged networks
    if (data.gpsNetworks >= 100 && !hasAchievement(ACH_GPS_MASTER)) {
        unlockAchievement(ACH_GPS_MASTER);
    }
    
    // 50km total walked
    if (data.lifetimeDistance >= 50000 && !hasAchievement(ACH_TOUCH_GRASS)) {
        unlockAchievement(ACH_TOUCH_GRASS);
    }
    
    // 5000 lifetime networks
    if (data.lifetimeNetworks >= 5000 && !hasAchievement(ACH_SILICON_PSYCHO)) {
        unlockAchievement(ACH_SILICON_PSYCHO);
    }
    
    // 1000 BLE packets
    if (data.lifetimeBLE >= 1000 && !hasAchievement(ACH_CHAOS_AGENT)) {
        unlockAchievement(ACH_CHAOS_AGENT);
    }
}

const PorkXPData& XP::getData() {
    return data;
}

const SessionStats& XP::getSession() {
    return session;
}

void XP::setLevelUpCallback(void (*callback)(uint8_t, uint8_t)) {
    levelUpCallback = callback;
}

void XP::drawBar(M5Canvas& canvas) {
    // Draw XP bar at bottom of main canvas (y=91, in the empty space)
    // Format: "L## TITLE_FULL      ######.......... 100%"
    // Progress bar and percentage aligned to right edge
    int barY = 91;
    
    canvas.setTextSize(1);
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(top_left);
    
    // Calculate right-aligned elements first
    const int BAR_LEN = 12;  // 12 chars to fit longer titles
    uint8_t progress = getProgress();
    int filledBlocks = (progress * BAR_LEN + 50) / 100;  // Round to nearest
    
    // Build bar string: # for filled, . for empty
    char barStr[20];
    for (int i = 0; i < BAR_LEN; i++) {
        barStr[i] = (i < filledBlocks) ? '#' : '.';
    }
    barStr[BAR_LEN] = '\0';
    
    // Percentage (4 chars max: "100%")
    char pctStr[8];
    snprintf(pctStr, sizeof(pctStr), "%d%%", progress);
    int pctW = canvas.textWidth("100%");  // Fixed width for alignment
    
    // Position from right edge
    int pctX = DISPLAY_W - 2 - pctW;
    int barW = canvas.textWidth(barStr);
    int barX = pctX - 3 - barW;  // 3px gap before percentage
    
    // Draw percentage right-aligned
    canvas.setTextDatum(top_right);
    canvas.drawString(pctStr, DISPLAY_W - 2, barY);
    
    // Draw progress bar
    canvas.setTextDatum(top_left);
    canvas.drawString(barStr, barX, barY);
    
    // Level number - left side
    char levelStr[8];
    snprintf(levelStr, sizeof(levelStr), "L%d", getLevel());
    int levelW = canvas.textWidth(levelStr);
    canvas.drawString(levelStr, 2, barY);
    
    // Title - fill space between level and bar (no truncation needed now)
    const char* title = getTitle();
    String titleStr = title;
    int titleX = 2 + levelW + 4;  // 4px gap after level
    int maxTitleW = barX - titleX - 4;  // Available space for title
    
    // Truncate only if really needed
    while (canvas.textWidth(titleStr) > maxTitleW && titleStr.length() > 3) {
        titleStr = titleStr.substring(0, titleStr.length() - 1);
    }
    if (titleStr.length() < strlen(title)) {
        titleStr = titleStr.substring(0, titleStr.length() - 2) + "..";
    }
    canvas.drawString(titleStr, titleX, barY);
}
