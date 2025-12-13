// Porkchop RPG XP and Leveling System Implementation

#include "xp.h"
#include "../ui/display.h"
#include "../ui/swine_stats.h"
#include <M5Unified.h>

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
    5,      // NETWORK_WEP (rare find!)
    50,     // HANDSHAKE_CAPTURED
    75,     // PMKID_CAPTURED
    2,      // DEAUTH_SENT
    15,     // DEAUTH_SUCCESS
    2,      // WARHOG_LOGGED
    25,     // DISTANCE_KM
    2,      // BLE_BURST (was 1, doc says 2)
    3,      // BLE_APPLE
    2,      // BLE_ANDROID
    2,      // BLE_SAMSUNG
    2,      // BLE_WINDOWS
    5,      // GPS_LOCK (was 10, doc says 5)
    25,     // ML_ROGUE_DETECTED
    10,     // SESSION_30MIN (was 50, doc says 10)
    25,     // SESSION_60MIN (was 100, doc says 25)
    50,     // SESSION_120MIN (was 200, doc says 50)
    20      // LOW_BATTERY_CAPTURE
};

// 8 class names (every 5 levels)
static const char* CLASS_NAMES[] = {
    "SH0AT",     // L1-5
    "SN1FF3R",   // L6-10
    "PWNER",     // L11-15
    "R00T",      // L16-20
    "R0GU3",     // L21-25
    "EXPL01T",   // L26-30
    "WARL0RD",   // L31-35
    "L3G3ND"     // L36-40
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

// Achievement names (must match enum order)
static const char* ACHIEVEMENT_NAMES[] = {
    // Original 17 (bits 0-16)
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
    "CHAOS AGENT",
    "N13TZSCH3",
    "T3N THOU$AND",
    "N3WB SNIFFER",
    "500 P1GS",
    "OPEN S3ASON",
    "WEP L0LZER",
    "HANDSHAK3 HAM",
    "F1FTY SHAKES",
    "PMK1D F1END",
    "TR1PLE THREAT",
    "H0T STREAK",
    "F1RST D3AUTH",
    "DEAUTH TH0USAND",
    "RAMPAGE",
    "HALF MARAT0N",
    "HUNDRED K1L0",
    "GPS ADDICT",
    "ULTRAMAR4THON",
    "PARANOID ANDR01D",
    "SAMSUNG SPR4Y",
    "W1ND0WS PANIC",
    "BLE B0MBER",
    "OINK4GEDDON",
    "SESS10N V3T",
    "4 HOUR GR1ND",
    "EARLY B1RD",
    "W33KEND WARR10R",
    "R0GUE SP0TTER",
    "H1DDEN MASTER",
    "WPA3 HUNT3R",
    "MAX L3VEL"
};
static const uint8_t ACHIEVEMENT_COUNT = sizeof(ACHIEVEMENT_NAMES) / sizeof(ACHIEVEMENT_NAMES[0]);

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
    // Read achievements as two 32-bit values for uint64_t
    uint32_t achLow = prefs.getUInt("achieve", 0);
    uint32_t achHigh = prefs.getUInt("achievehi", 0);
    data.achievements = ((uint64_t)achHigh << 32) | achLow;
    data.lifetimeNetworks = prefs.getUInt("networks", 0);
    data.lifetimeHS = prefs.getUInt("hs", 0);
    data.lifetimePMKID = prefs.getUInt("pmkid", 0);
    data.lifetimeDeauths = prefs.getUInt("deauths", 0);
    data.lifetimeDistance = prefs.getUInt("distance", 0);
    data.lifetimeBLE = prefs.getUInt("ble", 0);
    data.hiddenNetworks = prefs.getUInt("hidden", 0);
    data.wpa3Networks = prefs.getUInt("wpa3", 0);
    data.gpsNetworks = prefs.getUInt("gpsnet", 0);
    data.openNetworks = prefs.getUInt("open", 0);
    data.androidBLE = prefs.getUInt("android", 0);
    data.samsungBLE = prefs.getUInt("samsung", 0);
    data.windowsBLE = prefs.getUInt("windows", 0);
    data.sessions = prefs.getUShort("sessions", 0);
    data.wepFound = prefs.getBool("wep", false);
    data.cachedLevel = calculateLevel(data.totalXP);
    
    prefs.end();
}

void XP::save() {
    prefs.begin("porkxp", false);  // Read-write
    
    prefs.putUInt("totalxp", data.totalXP);
    // Store achievements as two 32-bit values for uint64_t
    prefs.putUInt("achieve", (uint32_t)(data.achievements & 0xFFFFFFFF));
    prefs.putUInt("achievehi", (uint32_t)(data.achievements >> 32));
    prefs.putUInt("networks", data.lifetimeNetworks);
    prefs.putUInt("hs", data.lifetimeHS);
    prefs.putUInt("pmkid", data.lifetimePMKID);
    prefs.putUInt("deauths", data.lifetimeDeauths);
    prefs.putUInt("distance", data.lifetimeDistance);
    prefs.putUInt("ble", data.lifetimeBLE);
    prefs.putUInt("hidden", data.hiddenNetworks);
    prefs.putUInt("wpa3", data.wpa3Networks);
    prefs.putUInt("gpsnet", data.gpsNetworks);
    prefs.putUInt("open", data.openNetworks);
    prefs.putUInt("android", data.androidBLE);
    prefs.putUInt("samsung", data.samsungBLE);
    prefs.putUInt("windows", data.windowsBLE);
    prefs.putUShort("sessions", data.sessions);
    prefs.putBool("wep", data.wepFound);
    
    prefs.end();
    
    Serial.printf("[XP] Saved - LV%d (%lu XP)\n", getLevel(), data.totalXP);
}

// Static for km tracking - needs to be reset on session start
static uint32_t lastKmAwarded = 0;

// Last significant XP gain popup state (avoid chatty +1/+2 events)
static uint16_t lastXPGainAmount = 0;
static uint32_t lastXPGainMs = 0;

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
            data.lifetimeNetworks++;
            session.networks++;
            if (session.firstNetworkTime == 0) session.firstNetworkTime = millis();
            break;
        case XPEvent::NETWORK_OPEN:
            data.lifetimeNetworks++;
            data.openNetworks++;  // Track open networks
            session.networks++;
            if (session.firstNetworkTime == 0) session.firstNetworkTime = millis();
            break;
        case XPEvent::NETWORK_HIDDEN:
            data.lifetimeNetworks++;
            data.hiddenNetworks++;
            session.networks++;
            if (session.firstNetworkTime == 0) session.firstNetworkTime = millis();
            break;
        case XPEvent::NETWORK_WPA3:
            data.lifetimeNetworks++;
            data.wpa3Networks++;
            session.networks++;
            if (session.firstNetworkTime == 0) session.firstNetworkTime = millis();
            break;
        case XPEvent::NETWORK_WEP:
            data.lifetimeNetworks++;
            data.wepFound = true;  // Track WEP found (ancient relic!)
            session.networks++;
            if (session.firstNetworkTime == 0) session.firstNetworkTime = millis();
            break;
        case XPEvent::HANDSHAKE_CAPTURED:
            data.lifetimeHS++;
            session.handshakes++;
            // Check for clutch capture (handshake at <10% battery)
            if (M5.Power.getBatteryLevel() < 10 && !hasAchievement(ACH_CLUTCH_CAPTURE)) {
                unlockAchievement(ACH_CLUTCH_CAPTURE);
            }
            break;
        case XPEvent::PMKID_CAPTURED:
            data.lifetimeHS++;
            data.lifetimePMKID++;
            session.handshakes++;
            // Check for clutch capture (PMKID at <10% battery)
            if (M5.Power.getBatteryLevel() < 10 && !hasAchievement(ACH_CLUTCH_CAPTURE)) {
                unlockAchievement(ACH_CLUTCH_CAPTURE);
            }
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
            data.lifetimeBLE++;
            session.blePackets++;
            break;
        case XPEvent::BLE_APPLE:
            data.lifetimeBLE++;
            session.blePackets++;
            break;
        case XPEvent::BLE_ANDROID:
            data.lifetimeBLE++;
            data.androidBLE++;
            session.blePackets++;
            break;
        case XPEvent::BLE_SAMSUNG:
            data.lifetimeBLE++;
            data.samsungBLE++;
            session.blePackets++;
            break;
        case XPEvent::BLE_WINDOWS:
            data.lifetimeBLE++;
            data.windowsBLE++;
            session.blePackets++;
            break;
        case XPEvent::GPS_LOCK:
            session.gpsLockAwarded = true;
            break;
        case XPEvent::ML_ROGUE_DETECTED:
            // Rogue AP detected by ML - unlock achievement
            if (!session.rogueSpotterAwarded && !hasAchievement(ACH_ROGUE_SPOTTER)) {
                unlockAchievement(ACH_ROGUE_SPOTTER);
                session.rogueSpotterAwarded = true;
            }
            break;
        default:
            break;
    }
    
    // Apply capture XP multiplier for handshakes/PMKIDs (class buff: CR4CK_NOSE)
    if (event == XPEvent::HANDSHAKE_CAPTURED || event == XPEvent::PMKID_CAPTURED) {
        float captureMult = SwineStats::getCaptureXPMultiplier();
        amount = (uint16_t)(amount * captureMult);
        if (amount < 1) amount = 1;
    }
    
    // Apply distance XP multiplier for km walked (class buff: R04D_H0G)
    if (event == XPEvent::DISTANCE_KM) {
        float distMult = SwineStats::getDistanceXPMultiplier();
        amount = (uint16_t)(amount * distMult);
        if (amount < 1) amount = 1;
    }
    
    addXP(amount);
    checkAchievements();
}

void XP::addXP(uint16_t amount) {
    // Apply buff/debuff XP multiplier (SNOUT$HARP +25%, F0GSNOUT -15%)
    float mult = SwineStats::getXPMultiplier();
    uint16_t modifiedAmount = (uint16_t)(amount * mult);
    if (modifiedAmount < 1) modifiedAmount = 1;  // Always at least 1 XP
    
    uint8_t oldLevel = data.cachedLevel;
    
    data.totalXP += modifiedAmount;
    session.xp += modifiedAmount;

    // Record last significant XP gain for UI (show +XP<N> under the bar)
    if (modifiedAmount > 2) {
        lastXPGainAmount = modifiedAmount;
        lastXPGainMs = millis();
    }
    
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

PorkClass XP::getClass() {
    return getClassForLevel(getLevel());
}

PorkClass XP::getClassForLevel(uint8_t level) {
    if (level >= 36) return PorkClass::L3G3ND;
    if (level >= 31) return PorkClass::WARL0RD;
    if (level >= 26) return PorkClass::EXPL01T;
    if (level >= 21) return PorkClass::R0GU3;
    if (level >= 16) return PorkClass::R00T;
    if (level >= 11) return PorkClass::PWNER;
    if (level >= 6)  return PorkClass::SN1FF3R;
    return PorkClass::SH0AT;
}

const char* XP::getClassName() {
    return getClassNameFor(getClass());
}

const char* XP::getClassNameFor(PorkClass cls) {
    uint8_t idx = static_cast<uint8_t>(cls);
    if (idx > 7) idx = 7;
    return CLASS_NAMES[idx];
}

uint8_t XP::getClassIndex() {
    return static_cast<uint8_t>(getClass());
}

void XP::unlockAchievement(PorkAchievement ach) {
    if (hasAchievement(ach)) return;
    
    data.achievements |= ach;
    
    // Find achievement index for name lookup (count trailing zeros)
    uint8_t idx = 0;
    uint64_t mask = 1ULL;
    while (mask < (uint64_t)ach && idx < ACHIEVEMENT_COUNT - 1) {
        mask <<= 1;
        idx++;
    }
    
    Serial.printf("[XP] Achievement unlocked: %s\n", ACHIEVEMENT_NAMES[idx]);
    
    // Save immediately to persist the achievement
    save();
}

bool XP::hasAchievement(PorkAchievement ach) {
    return (data.achievements & ach) != 0;
}

uint64_t XP::getAchievements() {
    return data.achievements;
}

uint8_t XP::getUnlockedCount() {
    uint8_t count = 0;
    uint64_t ach = data.achievements;
    while (ach) {
        count += ach & 1;
        ach >>= 1;
    }
    return count;
}

const char* XP::getAchievementName(PorkAchievement ach) {
    uint8_t idx = 0;
    uint64_t mask = 1ULL;
    while (mask < (uint64_t)ach && idx < ACHIEVEMENT_COUNT - 1) {
        mask <<= 1;
        idx++;
    }
    return ACHIEVEMENT_NAMES[idx];
}

void XP::checkAchievements() {
    // ===== ORIGINAL 17 ACHIEVEMENTS =====
    
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
    
    // PMKID captured
    if (data.lifetimePMKID >= 1 && !hasAchievement(ACH_PMKID_HUNTER)) {
        unlockAchievement(ACH_PMKID_HUNTER);
    }
    
    // 50 networks in 10 minutes (600000ms)
    if (session.networks >= 50 && session.firstNetworkTime > 0 && !hasAchievement(ACH_SPEED_RUN)) {
        uint32_t elapsed = millis() - session.firstNetworkTime;
        if (elapsed <= 600000) {
            unlockAchievement(ACH_SPEED_RUN);
        }
    }
    
    // Hunt after midnight (check system time if valid)
    if (!session.nightOwlAwarded && !hasAchievement(ACH_NIGHT_OWL)) {
        time_t now = time(nullptr);
        if (now > 1700000000) {  // Valid time (after 2023)
            struct tm* timeinfo = localtime(&now);
            if (timeinfo && timeinfo->tm_hour >= 0 && timeinfo->tm_hour < 5) {
                // It's between midnight and 5am
                unlockAchievement(ACH_NIGHT_OWL);
                session.nightOwlAwarded = true;
            }
        }
    }
    
    // ===== NEW 30 ACHIEVEMENTS =====
    
    // --- Network milestones ---
    // 10,000 networks lifetime
    if (data.lifetimeNetworks >= 10000 && !hasAchievement(ACH_TEN_THOUSAND)) {
        unlockAchievement(ACH_TEN_THOUSAND);
    }
    
    // First 10 networks
    if (data.lifetimeNetworks >= 10 && !hasAchievement(ACH_NEWB_SNIFFER)) {
        unlockAchievement(ACH_NEWB_SNIFFER);
    }
    
    // 500 networks in session
    if (session.networks >= 500 && !hasAchievement(ACH_FIVE_HUNDRED)) {
        unlockAchievement(ACH_FIVE_HUNDRED);
    }
    
    // 50 open networks
    if (data.openNetworks >= 50 && !hasAchievement(ACH_OPEN_SEASON)) {
        unlockAchievement(ACH_OPEN_SEASON);
    }
    
    // WEP network found
    if (data.wepFound && !hasAchievement(ACH_WEP_LOLZER)) {
        unlockAchievement(ACH_WEP_LOLZER);
    }
    
    // --- Handshake/PMKID milestones ---
    // 10 handshakes lifetime
    if (data.lifetimeHS >= 10 && !hasAchievement(ACH_HANDSHAKE_HAM)) {
        unlockAchievement(ACH_HANDSHAKE_HAM);
    }
    
    // 50 handshakes lifetime
    if (data.lifetimeHS >= 50 && !hasAchievement(ACH_FIFTY_SHAKES)) {
        unlockAchievement(ACH_FIFTY_SHAKES);
    }
    
    // 10 PMKIDs captured
    if (data.lifetimePMKID >= 10 && !hasAchievement(ACH_PMKID_FIEND)) {
        unlockAchievement(ACH_PMKID_FIEND);
    }
    
    // 3 handshakes in session
    if (session.handshakes >= 3 && !hasAchievement(ACH_TRIPLE_THREAT)) {
        unlockAchievement(ACH_TRIPLE_THREAT);
    }
    
    // 5 handshakes in session
    if (session.handshakes >= 5 && !hasAchievement(ACH_HOT_STREAK)) {
        unlockAchievement(ACH_HOT_STREAK);
    }
    
    // --- Deauth milestones ---
    // First deauth
    if (data.lifetimeDeauths >= 1 && !hasAchievement(ACH_FIRST_DEAUTH)) {
        unlockAchievement(ACH_FIRST_DEAUTH);
    }
    
    // 1000 deauths
    if (data.lifetimeDeauths >= 1000 && !hasAchievement(ACH_DEAUTH_THOUSAND)) {
        unlockAchievement(ACH_DEAUTH_THOUSAND);
    }
    
    // 10 deauths in session
    if (session.deauths >= 10 && !hasAchievement(ACH_RAMPAGE)) {
        unlockAchievement(ACH_RAMPAGE);
    }
    
    // --- Distance/WARHOG milestones ---
    // 21km in session (half marathon)
    if (session.distanceM >= 21000 && !hasAchievement(ACH_HALF_MARATHON)) {
        unlockAchievement(ACH_HALF_MARATHON);
    }
    
    // 100km lifetime
    if (data.lifetimeDistance >= 100000 && !hasAchievement(ACH_HUNDRED_KM)) {
        unlockAchievement(ACH_HUNDRED_KM);
    }
    
    // 500 GPS-tagged networks
    if (data.gpsNetworks >= 500 && !hasAchievement(ACH_GPS_ADDICT)) {
        unlockAchievement(ACH_GPS_ADDICT);
    }
    
    // 50km in session (ultramarathon)
    if (session.distanceM >= 50000 && !hasAchievement(ACH_ULTRAMARATHON)) {
        unlockAchievement(ACH_ULTRAMARATHON);
    }
    
    // --- BLE/PIGGYBLUES milestones ---
    // 100 Android FastPair spam
    if (data.androidBLE >= 100 && !hasAchievement(ACH_PARANOID_ANDROID)) {
        unlockAchievement(ACH_PARANOID_ANDROID);
    }
    
    // 100 Samsung spam
    if (data.samsungBLE >= 100 && !hasAchievement(ACH_SAMSUNG_SPRAY)) {
        unlockAchievement(ACH_SAMSUNG_SPRAY);
    }
    
    // 100 Windows SwiftPair spam
    if (data.windowsBLE >= 100 && !hasAchievement(ACH_WINDOWS_PANIC)) {
        unlockAchievement(ACH_WINDOWS_PANIC);
    }
    
    // 5000 BLE packets
    if (data.lifetimeBLE >= 5000 && !hasAchievement(ACH_BLE_BOMBER)) {
        unlockAchievement(ACH_BLE_BOMBER);
    }
    
    // 10000 BLE packets
    if (data.lifetimeBLE >= 10000 && !hasAchievement(ACH_OINKAGEDDON)) {
        unlockAchievement(ACH_OINKAGEDDON);
    }
    
    // --- Time/session milestones ---
    // 100 sessions
    if (data.sessions >= 100 && !hasAchievement(ACH_SESSION_VET)) {
        unlockAchievement(ACH_SESSION_VET);
    }
    
    // 4 hour session (240 minutes = 14400000ms)
    if (!session.session240Awarded && !hasAchievement(ACH_FOUR_HOUR_GRIND)) {
        uint32_t sessionMinutes = (millis() - session.startTime) / 60000;
        if (sessionMinutes >= 240) {
            unlockAchievement(ACH_FOUR_HOUR_GRIND);
            session.session240Awarded = true;
        }
    }
    
    // Early bird (5-7am)
    if (!session.earlyBirdAwarded && !hasAchievement(ACH_EARLY_BIRD)) {
        time_t now = time(nullptr);
        if (now > 1700000000) {
            struct tm* timeinfo = localtime(&now);
            if (timeinfo && timeinfo->tm_hour >= 5 && timeinfo->tm_hour < 7) {
                unlockAchievement(ACH_EARLY_BIRD);
                session.earlyBirdAwarded = true;
            }
        }
    }
    
    // Weekend warrior (Saturday or Sunday)
    if (!session.weekendWarriorAwarded && !hasAchievement(ACH_WEEKEND_WARRIOR)) {
        time_t now = time(nullptr);
        if (now > 1700000000) {
            struct tm* timeinfo = localtime(&now);
            if (timeinfo && (timeinfo->tm_wday == 0 || timeinfo->tm_wday == 6)) {
                unlockAchievement(ACH_WEEKEND_WARRIOR);
                session.weekendWarriorAwarded = true;
            }
        }
    }
    
    // --- Special/rare ---
    // Rogue spotter is checked when ML_ROGUE_DETECTED event fires
    // (handled separately in addXP for ML events)
    
    // 50 hidden networks
    if (data.hiddenNetworks >= 50 && !hasAchievement(ACH_HIDDEN_MASTER)) {
        unlockAchievement(ACH_HIDDEN_MASTER);
    }
    
    // 25 WPA3 networks
    if (data.wpa3Networks >= 25 && !hasAchievement(ACH_WPA3_HUNTER)) {
        unlockAchievement(ACH_WPA3_HUNTER);
    }
    
    // Max level reached
    if (data.cachedLevel >= 40 && !hasAchievement(ACH_MAX_LEVEL)) {
        unlockAchievement(ACH_MAX_LEVEL);
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
    
    // Draw XP: label before progress bar
    int xpLabelW = canvas.textWidth("XP:");
    int xpLabelX = barX - xpLabelW - 2;  // 2px gap before bar
    canvas.setTextDatum(top_left);
    canvas.drawString("XP:", xpLabelX, barY);

    // Draw +XP<N> below the XP label (only for >2 XP increments)
    const uint32_t XP_GAIN_DISPLAY_MS = 1500;
    if (lastXPGainAmount > 2 && (millis() - lastXPGainMs) < XP_GAIN_DISPLAY_MS) {
        char gainStr[16];
        snprintf(gainStr, sizeof(gainStr), "+%u PTS!", (unsigned)lastXPGainAmount);
        canvas.drawString(gainStr, xpLabelX, barY + 8);
    }
    
    // Draw progress bar
    canvas.drawString(barStr, barX, barY);
    
    // Level number - left side
    char levelStr[8];
    snprintf(levelStr, sizeof(levelStr), "L%d", getLevel());
    int levelW = canvas.textWidth(levelStr);
    canvas.drawString(levelStr, 2, barY);
    
    // Title - fill space between level and XP: label
    const char* title = getTitle();
    String titleStr = title;
    int titleX = 2 + levelW + 4;  // 4px gap after level
    int maxTitleW = xpLabelX - titleX - 4;  // Available space for title
    
    // Truncate only if really needed
    while (canvas.textWidth(titleStr) > maxTitleW && titleStr.length() > 3) {
        titleStr = titleStr.substring(0, titleStr.length() - 1);
    }
    if (titleStr.length() < strlen(title)) {
        titleStr = titleStr.substring(0, titleStr.length() - 2) + "..";
    }
    canvas.drawString(titleStr, titleX, barY);
}
