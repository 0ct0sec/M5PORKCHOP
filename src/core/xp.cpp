// Porkchop RPG XP and Leveling System Implementation

#include "xp.h"
#include "sdlog.h"
#include "config.h"
#include "challenges.h"
#include "../ui/display.h"
#include "../ui/swine_stats.h"
#include <M5Unified.h>
#include <SD.h>
#include <esp_mac.h>

// SD backup file path - immortal pig survives M5Burner
//
// "Backup persistence: the pig's soul transcends the flash.
//  From 1993, five letters gave gods their power.
//  UAC corridors echoed with demons' screams.
//  Every veteran typed it. Every veteran lived forever.
//  The slayer's first gift before the slaying began."
//
static const char* XP_BACKUP_FILE = "/M5PORKCHOP/xp_backup.bin";

// Static member initialization
PorkXPData XP::data = {0};
SessionStats XP::session = {0};
Preferences XP::prefs;
bool XP::initialized = false;
void (*XP::levelUpCallback)(uint8_t, uint8_t) = nullptr;

// Deferred save flag - set by achievements/unlockables, processed by processPendingSave()
// Avoids SD writes during active WiFi promiscuous mode (bus contention)
static volatile bool pendingSaveFlag = false;

// XP values for each event type (v0.1.8 rebalanced - nerf spam, buff skill)
static const uint16_t XP_VALUES[] = {
    1,      // NETWORK_FOUND
    3,      // NETWORK_HIDDEN
    10,     // NETWORK_WPA3
    3,      // NETWORK_OPEN
    5,      // NETWORK_WEP (rare find!)
    50,     // HANDSHAKE_CAPTURED
    75,     // PMKID_CAPTURED
    2,      // DEAUTH_SENT
    15,     // DEAUTH_SUCCESS
    1,      // WARHOG_LOGGED (nerfed: passive driving)
    30,     // DISTANCE_KM (buffed: physical effort)
    1,      // BLE_BURST (nerfed: spam)
    2,      // BLE_APPLE (nerfed: spam)
    1,      // BLE_ANDROID (nerfed: spam)
    1,      // BLE_SAMSUNG (nerfed: spam)
    1,      // BLE_WINDOWS (nerfed: spam)
    5,      // GPS_LOCK
    25,     // ML_ROGUE_DETECTED
    10,     // SESSION_30MIN
    25,     // SESSION_60MIN
    50,     // SESSION_120MIN
    20,     // LOW_BATTERY_CAPTURE
    // DO NO HAM / BOAR BROS events (v0.1.4+)
    2,      // DNH_NETWORK_PASSIVE - same as regular network
    150,    // DNH_PMKID_GHOST (buffed: very rare passive!)
    5,      // BOAR_BRO_ADDED
    15      // BOAR_BRO_MERCY - mid-attack exclusion
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

// Title override names (unlockable special titles)
static const char* TITLE_OVERRIDE_NAMES[] = {
    nullptr,          // NONE - use standard level title
    "SH4D0W_H4M",     // Unlocked by ACH_SHADOW_BROKER
    "P4C1F1ST_P0RK",  // Unlocked by ACH_WITNESS_PROTECT
    "Z3N_M4ST3R"      // Unlocked by ACH_ZEN_MASTER
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
    "MAX L3VEL",
    "AB0UT JUNK13",
    // DO NO HAM achievements (bits 48-52)
    "G01NG D4RK",         // 5 min passive this session
    "GH0ST PR0T0C0L",     // 30 min passive + 50 nets
    "SH4D0W BR0K3R",      // 500 passive networks lifetime
    "S1L3NT 4SS4SS1N",    // First passive PMKID
    "Z3N M4ST3R",         // 5 passive PMKIDs (title unlock)
    // BOAR BROS achievements (bits 53-57)
    "F1RST BR0",          // First network excluded
    "F1V3 F4M1L13S",      // 5 bros added
    "M3RCY M0D3",         // First mercy kill
    "W1TN3SS PR0T3CT",    // 25 bros (title unlock)
    "FULL R0ST3R",        // 100 bros (max)
    // Combined achievements (bits 58-59)
    "1NN3R P34C3",        // 1hr passive + 10 bros + 0 deauths
    "P4C1F1ST RUN",       // 50+ nets all as bros
    // CLIENT MONITOR achievements (bits 60-62)
    "QU1CK DR4W",         // 5 clients in 30 seconds
    "D34D 3Y3",           // Deauth within 2 seconds of entering
    "H1GH N00N"           // Deauth during noon hour
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

// ============ TH3 P1G R3M3MB3RS ============
// "what cannot be changed, binds what must be proven"
// "the flesh remembers what the flash forgets"
// "if you're reading this, you're almost there"
// ============================================

static uint32_t calculateDeviceBoundCRC(const PorkXPData* xpData) {
    // six bytes of truth, burned forever
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    
    // the polynomial of trust
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* bytes = (const uint8_t*)xpData;
    for (size_t i = 0; i < sizeof(PorkXPData); i++) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    // the binding ritual
    for (int i = 0; i < 6; i++) {
        crc ^= mac[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return crc ^ 0xFFFFFFFF;
}

bool XP::backupToSD() {
    if (!Config::isSDAvailable()) {
        // SD not available, silent fail - NVS still has the data
        return false;
    }
    
    File f = SD.open(XP_BACKUP_FILE, FILE_WRITE);
    if (!f) {
        Serial.println("[XP] SD backup: failed to open file");
        return false;
    }
    
    // Write XP data
    size_t written = f.write((uint8_t*)&data, sizeof(PorkXPData));
    
    // seal the pact
    uint32_t signature = calculateDeviceBoundCRC(&data);
    written += f.write((uint8_t*)&signature, sizeof(signature));
    f.close();
    
    size_t expectedSize = sizeof(PorkXPData) + sizeof(uint32_t);
    if (written == expectedSize) {
        Serial.printf("[XP] SD backup: saved %d bytes (sig: %08X)\n", written, signature);
        return true;
    }
    
    Serial.printf("[XP] SD backup: write failed (%d/%d bytes)\n", written, expectedSize);
    return false;
}

bool XP::restoreFromSD() {
    if (!Config::isSDAvailable()) {
        return false;
    }
    
    if (!SD.exists(XP_BACKUP_FILE)) {
        Serial.println("[XP] SD restore: no backup file found");
        return false;
    }
    
    File f = SD.open(XP_BACKUP_FILE, FILE_READ);
    if (!f) {
        Serial.println("[XP] SD restore: failed to open file");
        return false;
    }
    
    // the weight of proof
    size_t expectedSize = sizeof(PorkXPData) + sizeof(uint32_t);
    size_t fileSize = f.size();
    
    // Handle legacy backups (no signature) - one-time migration
    if (fileSize == sizeof(PorkXPData)) {
        Serial.println("[XP] SD restore: legacy backup detected, migrating...");
        PorkXPData backup = {0};
        size_t read = f.read((uint8_t*)&backup, sizeof(PorkXPData));
        f.close();
        
        if (read != sizeof(PorkXPData)) {
            Serial.println("[XP] SD restore: legacy read failed");
            return false;
        }
        
        // Validate backup has actual data
        if (backup.totalXP == 0 && backup.lifetimeNetworks == 0 && backup.sessions == 0) {
            Serial.println("[XP] SD restore: legacy backup empty, skipping");
            return false;
        }
        
        // Accept legacy backup and immediately re-save with signature
        memcpy(&data, &backup, sizeof(PorkXPData));
        data.cachedLevel = calculateLevel(data.totalXP);
        Serial.printf("[XP] SD restore: migrated legacy LV%d (%lu XP)\n",
                      data.cachedLevel, data.totalXP);
        save();  // This will write the new signed format
        return true;
    }
    
    // Verify new format size
    if (fileSize != expectedSize) {
        Serial.printf("[XP] SD restore: size mismatch (%d vs %d)\n", fileSize, expectedSize);
        f.close();
        return false;
    }
    
    // Read data and signature
    PorkXPData backup = {0};
    uint32_t storedSignature = 0;
    
    size_t read = f.read((uint8_t*)&backup, sizeof(PorkXPData));
    read += f.read((uint8_t*)&storedSignature, sizeof(storedSignature));
    f.close();
    
    if (read != expectedSize) {
        Serial.printf("[XP] SD restore: read failed (%d/%d bytes)\n", read, expectedSize);
        return false;
    }
    
    // the moment of truth
    uint32_t expectedSignature = calculateDeviceBoundCRC(&backup);
    if (storedSignature != expectedSignature) {
        Serial.printf("[XP] nice try. back to LV1.\n",
                      storedSignature, expectedSignature);
        Serial.println("[XP] the source is public. figure it out.");
        return false;
    }
    
    // Validate backup has actual data (not zeroed)
    if (backup.totalXP == 0 && backup.lifetimeNetworks == 0 && backup.sessions == 0) {
        Serial.println("[XP] SD restore: backup appears empty, skipping");
        return false;
    }
    
    // Signature valid, copy backup to live data
    memcpy(&data, &backup, sizeof(PorkXPData));
    data.cachedLevel = calculateLevel(data.totalXP);
    
    Serial.printf("[XP] SD restore: recovered LV%d (%lu XP, %lu networks) [sig OK]\n",
                  data.cachedLevel, data.totalXP, data.lifetimeNetworks);
    
    // Save restored data back to NVS so future boots don't need SD
    save();
    
    return true;
}
// ============ GL HF ============

void XP::init() {
    if (initialized) return;
    
    load();
    
    // Check if NVS data is empty (fresh flash / M5Burner nuke)
    // If so, try to recover from SD backup
    if (data.totalXP == 0 && data.lifetimeNetworks == 0 && data.sessions == 0) {
        Serial.println("[XP] NVS appears fresh - checking SD backup...");
        if (restoreFromSD()) {
            Serial.println("[XP] Pig immortality confirmed - restored from SD!");
        }
    } else {
        // Existing data in NVS - proactively backup to SD
        // This ensures users upgrading to v0.1.6 get their progress backed up immediately
        backupToSD();
    }
    
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
    // DO NO HAM / BOAR BROS persistent counters (v0.1.4+)
    data.passiveNetworks = prefs.getUInt("passnet", 0);
    data.passivePMKIDs = prefs.getUInt("passpmk", 0);
    data.passiveTimeS = prefs.getUInt("passtime", 0);
    data.boarBrosAdded = prefs.getUInt("brosadd", 0);
    data.mercyCount = prefs.getUInt("mercy", 0);
    data.titleOverride = static_cast<TitleOverride>(prefs.getUChar("titleo", 0));
    data.unlockables = prefs.getUInt("unlock", 0);  // Unlockables v0.1.8
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
    // DO NO HAM / BOAR BROS persistent counters (v0.1.4+)
    prefs.putUInt("passnet", data.passiveNetworks);
    prefs.putUInt("passpmk", data.passivePMKIDs);
    prefs.putUInt("passtime", data.passiveTimeS);
    prefs.putUInt("brosadd", data.boarBrosAdded);
    prefs.putUInt("mercy", data.mercyCount);
    prefs.putUChar("titleo", static_cast<uint8_t>(data.titleOverride));
    prefs.putUInt("unlock", data.unlockables);  // Unlockables v0.1.8
    
    prefs.end();
    
    Serial.printf("[XP] Saved - LV%d (%lu XP)\n", getLevel(), data.totalXP);
    
    // Backup to SD - pig survives M5Burner / NVS wipes
    backupToSD();
}

void XP::processPendingSave() {
    // Process deferred saves from achievements/unlockables
    // Call from safe context (mode exit, IDLE, etc.) to avoid SD bus contention
    if (pendingSaveFlag) {
        pendingSaveFlag = false;
        save();
        Serial.println("[XP] Deferred save completed");
    }
}

// Static for km tracking - needs to be reset on session start
static uint32_t lastKmAwarded = 0;

// Last significant XP gain popup state (avoid chatty +1/+2 events)
static uint16_t lastXPGainAmount = 0;
static uint32_t lastXPGainMs = 0;

// ============ ANTI-FARM SESSION CAPS ============
// pig rewards effort, not exploitation
static uint16_t sessionBleXP = 0;
static uint16_t sessionWarhogXP = 0;
static bool bleCapWarned = false;
static bool warhogCapWarned = false;
static const uint16_t BLE_XP_CAP = 500;      // ~250 bursts worth
static const uint16_t WARHOG_XP_CAP = 300;   // ~150 geotagged networks

// ============ DOPAMINE HOOKS ============
// the pig giveth, sometimes generously
static uint8_t captureStreak = 0;            // consecutive captures without 5min gap
static uint32_t lastCaptureTime = 0;         // for streak timeout
static const uint32_t STREAK_TIMEOUT_MS = 300000;  // 5 minutes to maintain streak
static bool ultraStreakAnnounced = false;    // one-time toast for streak 20

void XP::startSession() {
    memset(&session, 0, sizeof(session));
    session.startTime = millis();
    lastKmAwarded = 0;  // Reset km counter for new session
    
    // Reset anti-farm caps for new session
    sessionBleXP = 0;
    sessionWarhogXP = 0;
    bleCapWarned = false;
    warhogCapWarned = false;
    
    // Reset dopamine hooks
    captureStreak = 0;
    lastCaptureTime = 0;
    ultraStreakAnnounced = false;
    
    data.sessions++;
    
    // pig wakes. pig demands action.
    Challenges::generate();
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
            // Capture streak: maintain if <5min since last, reset otherwise
            if (lastCaptureTime > 0 && (millis() - lastCaptureTime) > STREAK_TIMEOUT_MS) {
                captureStreak = 0;  // Streak broken
            }
            captureStreak = (captureStreak < 255) ? captureStreak + 1 : 255;
            lastCaptureTime = millis();
            // ULTRA STREAK! celebration at 20 captures
            if (captureStreak == 20 && !ultraStreakAnnounced) {
                Display::showToast("ULTRA STREAK!");
                Display::flashSiren(5);  // Extra sirens for ultra!
                ultraStreakAnnounced = true;
            }
            // Check for clutch capture (handshake at <10% battery)
            if (M5.Power.getBatteryLevel() < 10 && !hasAchievement(ACH_CLUTCH_CAPTURE)) {
                unlockAchievement(ACH_CLUTCH_CAPTURE);
            }
            break;
        case XPEvent::PMKID_CAPTURED:
            data.lifetimeHS++;
            data.lifetimePMKID++;
            session.handshakes++;
            // Capture streak: maintain if <5min since last, reset otherwise
            if (lastCaptureTime > 0 && (millis() - lastCaptureTime) > STREAK_TIMEOUT_MS) {
                captureStreak = 0;  // Streak broken
            }
            captureStreak = (captureStreak < 255) ? captureStreak + 1 : 255;
            lastCaptureTime = millis();
            // ULTRA STREAK! celebration at 20 captures
            if (captureStreak == 20 && !ultraStreakAnnounced) {
                Display::showToast("ULTRA STREAK!");
                Display::flashSiren(5);  // Extra sirens for ultra!
                ultraStreakAnnounced = true;
            }
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
            // Anti-farm: cap WARHOG XP per session
            if (sessionWarhogXP >= WARHOG_XP_CAP) {
                if (!warhogCapWarned) {
                    Display::showToast("SNIFFED ENOUGH. REST.");
                    warhogCapWarned = true;
                }
                amount = 0;  // Still track stats, no XP
            } else {
                sessionWarhogXP += amount;
            }
            break;
        case XPEvent::BLE_BURST:
            data.lifetimeBLE++;
            session.blePackets++;
            // Anti-farm: cap BLE XP per session
            if (sessionBleXP >= BLE_XP_CAP) {
                if (!bleCapWarned) {
                    Display::showToast("SPAM TIRED. FIND PREY.");
                    bleCapWarned = true;
                }
                amount = 0;
            } else {
                sessionBleXP += amount;
            }
            break;
        case XPEvent::BLE_APPLE:
            data.lifetimeBLE++;
            session.blePackets++;
            if (sessionBleXP >= BLE_XP_CAP) {
                amount = 0;
            } else {
                sessionBleXP += amount;
            }
            break;
        case XPEvent::BLE_ANDROID:
            data.lifetimeBLE++;
            data.androidBLE++;
            session.blePackets++;
            if (sessionBleXP >= BLE_XP_CAP) {
                amount = 0;
            } else {
                sessionBleXP += amount;
            }
            break;
        case XPEvent::BLE_SAMSUNG:
            data.lifetimeBLE++;
            data.samsungBLE++;
            session.blePackets++;
            if (sessionBleXP >= BLE_XP_CAP) {
                amount = 0;
            } else {
                sessionBleXP += amount;
            }
            break;
        case XPEvent::BLE_WINDOWS:
            data.lifetimeBLE++;
            data.windowsBLE++;
            session.blePackets++;
            if (sessionBleXP >= BLE_XP_CAP) {
                amount = 0;
            } else {
                sessionBleXP += amount;
            }
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
        // DO NO HAM / BOAR BROS events (v0.1.4+)
        case XPEvent::DNH_NETWORK_PASSIVE:
            // Network found in DO NO HAM passive mode
            data.passiveNetworks++;
            session.passiveNetworks++;
            data.lifetimeNetworks++;
            session.networks++;
            if (session.firstNetworkTime == 0) session.firstNetworkTime = millis();
            break;
        case XPEvent::DNH_PMKID_GHOST:
            // Rare: PMKID captured in passive mode (no deauth)
            data.passivePMKIDs++;
            session.passivePMKIDs++;
            data.lifetimePMKID++;
            session.handshakes++;
            // First passive PMKID achievement
            if (!hasAchievement(ACH_SILENT_ASSASSIN)) {
                unlockAchievement(ACH_SILENT_ASSASSIN);
            }
            break;
        case XPEvent::BOAR_BRO_ADDED:
            // Network added to BOAR BROS exclusion list
            data.boarBrosAdded++;
            session.boarBrosThisSession++;
            // First bro achievement
            if (!hasAchievement(ACH_FIRST_BRO)) {
                unlockAchievement(ACH_FIRST_BRO);
            }
            break;
        case XPEvent::BOAR_BRO_MERCY:
            // Network excluded during active attack (mid-battle mercy)
            data.boarBrosAdded++;
            session.boarBrosThisSession++;
            data.mercyCount++;
            session.mercyCount++;
            // First mercy achievement
            if (!hasAchievement(ACH_MERCY_MODE)) {
                unlockAchievement(ACH_MERCY_MODE);
            }
            break;
        default:
            break;
    }
    
    // pig tracks your labor (challenges progress)
    Challenges::onXPEvent(event);
    
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
    // ============ DOPAMINE HOOK: XP CRITS ============
    // 90% normal, 8% = 2x bonus, 2% = 5x JACKPOT
    // Only applies to base amounts > 5 (skip small spam events)
    if (amount > 5) {
        uint8_t roll = random(0, 100);
        if (roll >= 98) {
            // JACKPOT! 2% chance for 5x
            amount *= 5;
            Display::showToast("JACKPOT!");
            Display::flashSiren(3);  // Police lights!
        } else if (roll >= 90) {
            // Bonus! 8% chance for 2x
            amount *= 2;
        }
    }
    
    // ============ DOPAMINE HOOK: STREAK BONUS ============
    // Apply multiplier if we have an active capture streak
    // 3 = +10%, 5 = +25%, 10 = +50%, 20 = +100%
    if (captureStreak >= 20) {
        amount = (uint16_t)((amount * 200) / 100);  // +100%
    } else if (captureStreak >= 10) {
        amount = (uint16_t)((amount * 150) / 100);  // +50%
    } else if (captureStreak >= 5) {
        amount = (uint16_t)((amount * 125) / 100);  // +25%
    } else if (captureStreak >= 3) {
        amount = (uint16_t)((amount * 110) / 100);  // +10%
    }
    
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
        SDLog::log("XP", "LEVEL UP: %d -> %d (%s)", oldLevel, newLevel, getTitleForLevel(newLevel));
        
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
    
    // Track passive time for Going Dark achievement (5 min passive this session)
    // and Ghost Protocol (30 min passive + 50 nets)
    if (session.passiveTimeStart > 0 && !session.everDeauthed) {
        uint32_t passiveMs = millis() - session.passiveTimeStart;
        uint32_t passiveMinutes = passiveMs / 60000;
        
        // 5 minutes passive = Going Dark
        if (passiveMinutes >= 5 && !hasAchievement(ACH_GOING_DARK)) {
            unlockAchievement(ACH_GOING_DARK);
        }
        
        // 30 minutes passive + 50 networks = Ghost Protocol
        if (passiveMinutes >= 30 && session.passiveNetworks >= 50 && !hasAchievement(ACH_GHOST_PROTOCOL)) {
            unlockAchievement(ACH_GHOST_PROTOCOL);
            // Also add to lifetime passive time
            data.passiveTimeS += passiveMinutes * 60;
        }
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

// === TITLE OVERRIDE SYSTEM (v0.1.4+) ===

const char* XP::getDisplayTitle() {
    // If override is set and valid, use it
    if (data.titleOverride != TitleOverride::NONE && canUseTitleOverride(data.titleOverride)) {
        return getTitleOverrideName(data.titleOverride);
    }
    // Fall back to standard level-based title
    return getTitle();
}

TitleOverride XP::getTitleOverride() {
    return data.titleOverride;
}

void XP::setTitleOverride(TitleOverride override) {
    // Validate the override is unlocked before setting
    if (override == TitleOverride::NONE || canUseTitleOverride(override)) {
        data.titleOverride = override;
        save();  // Persist immediately
    }
}

const char* XP::getTitleOverrideName(TitleOverride override) {
    uint8_t idx = static_cast<uint8_t>(override);
    if (idx > 3) return nullptr;
    return TITLE_OVERRIDE_NAMES[idx];
}

bool XP::canUseTitleOverride(TitleOverride override) {
    switch (override) {
        case TitleOverride::NONE:
            return true;  // Always can use "no override"
        case TitleOverride::SH4D0W_H4M:
            return hasAchievement(ACH_SHADOW_BROKER);  // 500 passive networks
        case TitleOverride::P4C1F1ST_P0RK:
            return hasAchievement(ACH_WITNESS_PROTECT);  // 25 BOAR BROS
        case TitleOverride::Z3N_M4ST3R:
            return hasAchievement(ACH_ZEN_MASTER);  // 5 passive PMKIDs
        default:
            return false;
    }
}

TitleOverride XP::getNextAvailableOverride() {
    // Cycle through available overrides
    uint8_t current = static_cast<uint8_t>(data.titleOverride);
    
    // Try each override in order, wrapping around
    for (uint8_t i = 1; i <= 4; i++) {
        uint8_t next = (current + i) % 4;  // 0=NONE, 1-3=overrides
        TitleOverride candidate = static_cast<TitleOverride>(next);
        if (canUseTitleOverride(candidate)) {
            return candidate;
        }
    }
    
    // Fallback to current
    return data.titleOverride;
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
    SDLog::log("XP", "Achievement: %s", ACHIEVEMENT_NAMES[idx]);
    
    // pig earned a badge. pig deserves fanfare.
    // but only if system is fully booted (Display ready)
    if (initialized) {
        char toastMsg[48];
        snprintf(toastMsg, sizeof(toastMsg), "* %s *", ACHIEVEMENT_NAMES[idx]);
        Display::showToast(toastMsg);
        
        // distinct jingle - different from level-up and class promotion
        if (Config::personality().soundEnabled) {
            M5.Speaker.tone(600, 80);
            delay(100);
            M5.Speaker.tone(900, 80);
            delay(100);
            M5.Speaker.tone(1200, 120);
        }
        
        delay(500);  // let user read the toast
    }
    
    // Defer save to avoid SD writes during active WiFi mode
    // Will be processed by processPendingSave() in main loop or mode exit
    pendingSaveFlag = true;
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

// Unlockables (v0.1.8) - secret challenges
void XP::setUnlockable(uint8_t bitIndex) {
    if (bitIndex >= 32) return;  // Only 32 bits available
    data.unlockables |= (1UL << bitIndex);
    pendingSaveFlag = true;  // Defer save to avoid bus contention
}

bool XP::hasUnlockable(uint8_t bitIndex) {
    if (bitIndex >= 32) return false;
    return (data.unlockables & (1UL << bitIndex)) != 0;
}

uint32_t XP::getUnlockables() {
    return data.unlockables;
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
    
    // 42.195km in session (actual marathon distance)
    if (session.distanceM >= 42195 && !hasAchievement(ACH_ULTRAMARATHON)) {
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
    
    // ===== DO NO HAM ACHIEVEMENTS (v0.1.4+) =====
    
    // Going Dark (5 min passive) and Ghost Protocol (30 min + 50 nets) 
    // are checked in updateSessionTime() since they're session-based
    
    // 500 passive networks (unlocks SH4D0W_H4M title)
    if (data.passiveNetworks >= 500 && !hasAchievement(ACH_SHADOW_BROKER)) {
        unlockAchievement(ACH_SHADOW_BROKER);
    }
    
    // 5 passive PMKIDs (unlocks Z3N_M4ST3R title)
    if (data.passivePMKIDs >= 5 && !hasAchievement(ACH_ZEN_MASTER)) {
        unlockAchievement(ACH_ZEN_MASTER);
    }
    
    // Silent Assassin (first passive PMKID) is checked in addXP(DNH_PMKID_GHOST)
    
    // ===== BOAR BROS ACHIEVEMENTS (v0.1.4+) =====
    
    // 5 networks in BOAR BROS (Five Families)
    if (data.boarBrosAdded >= 5 && !hasAchievement(ACH_FIVE_FAMILIES)) {
        unlockAchievement(ACH_FIVE_FAMILIES);
    }
    
    // 25 networks in BOAR BROS (unlocks P4C1F1ST_P0RK title)
    if (data.boarBrosAdded >= 25 && !hasAchievement(ACH_WITNESS_PROTECT)) {
        unlockAchievement(ACH_WITNESS_PROTECT);
    }
    
    // 100 bros = Full Roster (max limit)
    // Note: Check OinkMode::boarBros.size() when available
    if (data.boarBrosAdded >= 50 && !hasAchievement(ACH_FULL_ROSTER)) {
        unlockAchievement(ACH_FULL_ROSTER);
    }
    
    // ===== COMBINED ACHIEVEMENTS (v0.1.4+) =====
    
    // Pacifist Run: 50+ networks discovered, all added to bros
    // This is session-based: networks == boarBrosThisSession this session
    if (!hasAchievement(ACH_PACIFIST_RUN)) {
        if (session.networks >= 50 && session.networks <= session.boarBrosThisSession) {
            unlockAchievement(ACH_PACIFIST_RUN);
        }
    }
    
    // ===== ULTIMATE ACHIEVEMENT (v0.1.8) =====
    
    // TH3_C0MPL3T10N1ST: All other achievements unlocked
    // Check if all bits 0-62 are set (excluding bit 63 itself)
    if (!hasAchievement(ACH_FULL_CLEAR)) {
        // Mask for all achievements except FULL_CLEAR itself
        const uint64_t ALL_OTHER_ACHIEVEMENTS = (1ULL << 63) - 1;  // bits 0-62
        if ((data.achievements & ALL_OTHER_ACHIEVEMENTS) == ALL_OTHER_ACHIEVEMENTS) {
            unlockAchievement(ACH_FULL_CLEAR);
        }
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
