// Porkchop RPG XP and Leveling System Implementation

#include "xp.h"
#include "sdlog.h"
#include "config.h"
#include "sd_layout.h"
#include "challenges.h"
#include "../ui/display.h"
#include "../ui/flexes_screen.h"
#include "../audio/sfx.h"
#include "../piglet/avatar.h"
#include <M5Unified.h>
#include <SD.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// SD backup file path - immortal pig survives M5Burner
//
// "Backup persistence: the pig's soul transcends the flash.
//  From 1993, five letters gave gods their power.
//  UAC corridors echoed with demons' screams.
//  Every veteran typed it. Every veteran lived forever.
//  The slayer's first gift before the slaying began."
//

// Static member initialization
PorkXPData XP::data = {0};
SessionStats XP::session = {0};
Preferences XP::prefs;
bool XP::initialized = false;
void (*XP::levelUpCallback)(uint8_t, uint8_t) = nullptr;

// Deferred save flag - set by achievements/unlockables, processed by processPendingSave()
// Avoids SD writes during active WiFi promiscuous mode (bus contention)
// Protected by achQueueMutex to prevent race conditions
static bool pendingSaveFlag = false;

static uint32_t lastSavedCRC = 0;

// ============ RETURN BONUS ============
// pig missed you. pig rewards loyalty.
static uint32_t lastSessionEpoch = 0;       // epoch seconds from NVS (last save)
static bool returnBonusChecked = false;      // one-shot per session

static uint32_t computeDataCRC(const PorkXPData* d) {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* bytes = (const uint8_t*)d;
    for (size_t i = 0; i < sizeof(PorkXPData); i++) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return crc ^ 0xFFFFFFFF;
}

// Mutex for protecting achievement queue operations AND pendingSaveFlag
static SemaphoreHandle_t achQueueMutex = nullptr;

// Achievement queue - prevents cascade of sounds/toasts when multiple unlock at once
// Achievements are queued and processed one per frame via processAchievementQueue()
static const uint8_t ACH_QUEUE_SIZE = 8;
static PorkAchievement achQueue[ACH_QUEUE_SIZE];
static uint8_t achQueueHead = 0;
static uint8_t achQueueTail = 0;
static uint32_t lastAchievementTime = 0;
static const uint32_t ACH_COOLDOWN_MS = 600;  // Min time between achievement celebrations

// XP values for each event type (vNext Neon Operator)
// We nerf spammy actions, buff physical effort and rare passive captures.
static const uint16_t XP_VALUES[] = {
    1,      // NETWORK_FOUND
    3,      // NETWORK_HIDDEN
    10,     // NETWORK_WPA3
    3,      // NETWORK_OPEN
    5,      // NETWORK_WEP (rare find!)
    50,     // HANDSHAKE_CAPTURED
    75,     // PMKID_CAPTURED
    1,      // DEAUTH_SENT (vNext nerf: reward restraint, no spam)
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
    15,     // BOAR_BRO_MERCY - mid-attack exclusion
    15,     // SMOKED_BACON - rare upload bonus
    // C5Lab / JanOS / JANUS HOG (v0.1.9+)
    2,      // SAE_COMMIT_SENT - SAE flood burst
    25,     // C5_CONNECTED - JanusHog board detected
    5       // C5_5GHZ_FOUND - 5GHz network via C5
};

// 10 class names (every 5 levels)
// vNext Neon Operator: updated class names to emphasize
// operator archetypes and reduce repetitiveness. Each name
// corresponds to a 5‑level tier (see getClassForLevel()).
static const char* CLASS_NAMES[] = {
    "SH0AT",        // L1‑5 : freshly booted
    "SN1FF3R",      // L6‑10: packet sniffer
    "PR0B3R",       // L11‑15: probe and scout
    "PWN3R",        // L16‑20: first real exploits
    "H4ND5H4K3R",   // L21‑25: handshake hunter
    "M1TM B0AR",    // L26‑30: man‑in‑the‑middle operator
    "R00T BR1STL3", // L31‑35: root‑level bristles
    "PMF W4RD3N",   // L36‑40: Protected Management Frame savvy
    "MLO L3G3ND",   // L41‑45: multi‑link operator legend
    "B4C0NM4NC3R"   // L46‑50: endgame myth
};

// Title override names (unlockable special titles)
static const char* TITLE_OVERRIDE_NAMES[] = {
    nullptr,          // NONE - use standard level title
    "SH4D0W_H4M",     // Unlocked by ACH_SHADOW_BROKER
    "P4C1F1ST_P0RK",  // Unlocked by ACH_WITNESS_PROTECT
    "Z3N_M4ST3R"      // Unlocked by ACH_ZEN_MASTER
};

// 50 rank titles - hacker/grindhouse/tarantino + pig flavor
// vNext Neon Operator: completely refreshed rank titles. Each level has its own
// short, leet‑flavoured moniker that implies progression without repetition. See
// the design doc for the thematic progression (Scout → Hunter → Operator → Legend).
static const char* RANK_TITLES[] = {
    // Tier 1 (Levels 1–5)
    "BACON N00B",    // Lv1
    "0INK Z3R0",     // Lv2
    "SCRIP7 H4M",    // Lv3
    "P1N6 P1GL3T",   // Lv4
    "NMAP NIBBL3",   // Lv5
    // Tier 2 (Levels 6–10)
    "PR0B3 P0RK",    // Lv6
    "CH4N CH0P",     // Lv7
    "B34C0N B0AR",   // Lv8
    "SS1D SN0UT",     // Lv9
    "P4CK3T PR0D",    // Lv10
    // Tier 3 (Levels 11–15)
    "4SS0C SW1N3",    // Lv11
    "EAP0L E4T3R",    // Lv12
    "PMK1D P1CK3R",   // Lv13
    "R5N R4Z0R",      // Lv14
    "C4PTUR3 C00K",    // Lv15
    // Tier 4 (Levels 16–20)
    "D34UTH DU3L",    // Lv16
    "0FFCH4N 0PS",    // Lv17
    "M1TM MUDP1G",    // Lv18
    "1NJ3CT J0K3",    // Lv19
    "5P00F CH3F",     // Lv20
    // Tier 5 (Levels 21–25)
    "SAE S1ZZL3",     // Lv21
    "PMF SH13LD",     // Lv22
    "TR4NS1T TR0T",   // Lv23
    "6GHZ GR1NT",      // Lv24
    "0WE 0NK",        // Lv25
    // Tier 6 (Levels 26–30)
    "C3RT CH0MP",     // Lv26
    "EAP-TLS TUSK",   // Lv27
    "R4D10 R4NG3R",   // Lv28
    "R04M M4ST3R",    // Lv29
    "EHT BR15TL3",    // Lv30
    // Tier 7 (Levels 31–35)
    "FR4G 4TT4CK",    // Lv31
    "KR4CK CRU5H",    // Lv32
    "DR4G0NBL00D",    // Lv33
    "C0R3DUMP P1G",    // Lv34
    "R00TK1T R1ND",    // Lv35
    // Tier 8 (Levels 36–40)
    "PHR4CK P1G",     // Lv36
    "2600 B0AR",      // Lv37
    "BLU3B0X H4M",    // Lv38
    "C0NS0L3 C0W",    // Lv39
    "0xDE4D B4C0N",    // Lv40
    // Tier 9 (Levels 41–45)
    "SPRAWL PR0XY",   // Lv41
    "N3UR0 N0S3",     // Lv42
    "ICEBR34K B0AR",   // Lv43
    "D3CK D1V3R",     // Lv44
    "C0RP N3TW0RK",    // Lv45
    // Tier 10 (Levels 46–50)
    "K3RN3L H0G",     // Lv46
    "SYSC4LL SW1N",    // Lv47
    "NULL M4TR1X",     // Lv48
    "R00T 0F R00T",    // Lv49
    "B4C0NM4NC3R"      // Lv50
};
static const uint8_t MAX_LEVEL = 50;

// XP thresholds for each level (shared by calculateLevel and getXPForLevel)
// Designed for: L1-5 quick, L6-20 steady, L21-50 grind
static const uint32_t XP_THRESHOLDS[50] = {
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
    359000,  // L36
    404000,  // L37
    454000,  // L38
    514000,  // L39
    600000,  // L40
    680000,  // L41
    770000,  // L42
    870000,  // L43
    980000,  // L44
    1100000, // L45
    1230000, // L46
    1370000, // L47
    1520000, // L48
    1680000, // L49
    1850000  // L50
};

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
    "PR0PH3CY W1TN3SS",  // Witnessed the riddle prophecy
    "P4C1F1ST RUN",       // 50+ nets all as bros
    // CLIENT MONITOR achievements (bits 60-62)
    "QU1CK DR4W",         // 5 clients in 30 seconds
    "D34D 3Y3",           // Deauth within 2 seconds of entering
    "H1GH N00N",          // Deauth during noon hour
    // Ultimate achievement (bit 63)
    "TH3_C0MPL3T10N1ST"
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

static uint32_t calculateDeviceBoundCRC(const PorkXPData* xpData, size_t dataSize = sizeof(PorkXPData)) {
    // six bytes of truth, burned forever
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    
    // the polynomial of trust
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* bytes = (const uint8_t*)xpData;
    for (size_t i = 0; i < dataSize; i++) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
        // Yield periodically to prevent WDT issues during long CRC calculations
        if (i % 100 == 0) {
            delay(1);  // Allow other tasks to run
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
    
    const char* backupPath = SDLayout::xpBackupPath();
    // Atomic write: write to .tmp, verify, rename over live file
    char tmpPath[128];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", backupPath);

    File f = SD.open(tmpPath, FILE_WRITE);
    if (!f) {
        Serial.println("[XP] SD backup: failed to open temp file");
        return false;
    }

    // Write XP data
    size_t written = f.write((uint8_t*)&data, sizeof(PorkXPData));

    // seal the pact
    uint32_t signature = calculateDeviceBoundCRC(&data);
    written += f.write((uint8_t*)&signature, sizeof(signature));
    f.close();

    size_t expectedSize = sizeof(PorkXPData) + sizeof(uint32_t);
    if (written != expectedSize) {
        Serial.printf("[XP] SD backup: write failed (%d/%d bytes)\n", written, expectedSize);
        SD.remove(tmpPath);
        return false;
    }

    // Atomic rename: single FAT32 directory entry update
    SD.remove(backupPath);
    if (!SD.rename(tmpPath, backupPath)) {
        Serial.printf("[XP] SD backup: rename failed\n");
        return false;
    }

    Serial.printf("[XP] SD backup: saved %d bytes (sig: %08X)\n", written, signature);
    return true;
}

bool XP::restoreFromSD() {
    if (!Config::isSDAvailable()) {
        return false;
    }
    
    const char* backupPath = SDLayout::xpBackupPath();
    if (!SD.exists(backupPath)) {
        Serial.println("[XP] SD restore: no backup file found");
        return false;
    }

    File f = SD.open(backupPath, FILE_READ);
    if (!f) {
        Serial.println("[XP] SD restore: failed to open file");
        return false;
    }
    
    size_t signatureSize = sizeof(uint32_t);
    size_t fileSize = f.size();
    
    if (fileSize <= signatureSize) {
        Serial.printf("[XP] SD restore: size too small (%d bytes)\n", fileSize);
        f.close();
        return false;
    }
    
    PorkXPData backup = {0};
    uint32_t storedSignature = 0;
    size_t dataSize = fileSize - signatureSize;
    bool signatureValid = false;
    
    if (dataSize <= sizeof(PorkXPData)) {
        size_t read = f.read((uint8_t*)&backup, dataSize);
        read += f.read((uint8_t*)&storedSignature, signatureSize);
        if (read == fileSize) {
            uint32_t expectedSignature = calculateDeviceBoundCRC(&backup, dataSize);
            signatureValid = (storedSignature == expectedSignature);
        }
    }
    
    if (!signatureValid) {
        if (fileSize > sizeof(PorkXPData)) {
            Serial.printf("[XP] SD restore: size mismatch (%d bytes)\n", fileSize);
            f.close();
            return false;
        }
        
        // Legacy unsigned backup
        f.seek(0);
        backup = {0};
        size_t read = f.read((uint8_t*)&backup, fileSize);
        if (read != fileSize) {
            Serial.println("[XP] SD restore: legacy read failed");
            f.close();
            return false;
        }
        
        // Validate backup has actual data
        if (backup.totalXP == 0 && backup.lifetimeNetworks == 0 && backup.sessions == 0) {
            Serial.println("[XP] SD restore: legacy backup empty, skipping");
            f.close();
            return false;
        }
        
        // Accept legacy backup and immediately re-save with signature
        memcpy(&data, &backup, sizeof(PorkXPData));
        data.cachedLevel = calculateLevel(data.totalXP);
        Serial.printf("[XP] SD restore: migrated legacy LV%d (%lu XP)\n",
                      data.cachedLevel, data.totalXP);
        f.close();
        save();  // This will write the new signed format
        return true;
    }
    
    f.close();
    
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
    
    // Initialize achievement queue mutex
    if (achQueueMutex == nullptr) {
        achQueueMutex = xSemaphoreCreateMutex();
    }
    
    // Initialize achievement queue
    achQueueHead = 0;
    achQueueTail = 0;
    lastAchievementTime = 0;
    
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
    data.rouletteWins = prefs.getUInt("roulette", 0);
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
    lastSessionEpoch = prefs.getUInt("lastsess", 0);  // Return bonus tracking
    data.cachedLevel = calculateLevel(data.totalXP);

    prefs.end();
    lastSavedCRC = computeDataCRC(&data);
}

void XP::save() {
    uint32_t currentCRC = computeDataCRC(&data);
    if (currentCRC == lastSavedCRC) return;

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
    prefs.putUInt("roulette", data.rouletteWins);
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
    // Update last session epoch for return bonus tracking
    {
        time_t now = time(nullptr);
        if (now > 1700000000) {
            prefs.putUInt("lastsess", (uint32_t)now);
            lastSessionEpoch = (uint32_t)now;
        }
    }

    prefs.end();
    lastSavedCRC = currentCRC;

    Serial.printf("[XP] Saved - LV%d (%lu XP)\n", getLevel(), data.totalXP);
    
    // Backup to SD - pig survives M5Burner / NVS wipes
    backupToSD();
}

void XP::processPendingSave() {
    // Process deferred saves from achievements/unlockables
    // Call from safe context (mode exit, IDLE, etc.) to avoid SD bus contention
    bool needsSave = false;
    
    // Protect flag access with mutex to prevent race with unlockAchievement()
    if (achQueueMutex != nullptr && xSemaphoreTake(achQueueMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        needsSave = pendingSaveFlag;
        if (needsSave) {
            pendingSaveFlag = false;
        }
        xSemaphoreGive(achQueueMutex);
    }
    
    if (needsSave) {
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
static const uint16_t WARHOG_XP_CAP = 800;   // ~400 geotagged networks (tuned: wardriving-viable progression)

// ============ DOPAMINE HOOKS ============
// the pig giveth, sometimes generously
static uint8_t captureStreak = 0;            // consecutive captures without 5min gap
static uint32_t lastCaptureTime = 0;         // for streak timeout
static const uint32_t STREAK_TIMEOUT_MS = 300000;  // 5 minutes to maintain streak
static bool ultraStreakAnnounced = false;    // one-time toast for streak 20

// Indicates whether the capture streak bonus should be applied for the current XP event.
// This flag is set in addXP(XPEvent) for capture-family events (handshakes, PMKIDs, passive PMKID ghosts)
// and read/cleared in addXP(uint16_t) when applying the streak multiplier.
static bool streakAppliesForCurrentEvent = false;

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
    returnBonusChecked = false;
    
    data.sessions++;
    
    // pig wakes. pig demands action.
    Challenges::generate();
}

void XP::addXP(XPEvent event) {
    uint8_t eventIdx = static_cast<uint8_t>(event);
    // Bounds check to prevent array access violation
    if (eventIdx >= sizeof(XP_VALUES) / sizeof(XP_VALUES[0])) {
        return; // Invalid event type, silently ignore
    }
    uint16_t amount = XP_VALUES[eventIdx];
    
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
                SFX::play(SFX::ULTRA_STREAK);  // Non-blocking celebration
                Avatar::triggerTailWiggle();
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
                SFX::play(SFX::ULTRA_STREAK);  // Non-blocking celebration
                Avatar::triggerTailWiggle();
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
                    Display::notify(NoticeKind::WARNING, "MAPS FULL. GO HUNT.");
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
                    Display::notify(NoticeKind::WARNING, "BLE MAXED. TRY OINK.");
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

            // Treat passive PMKID captures as part of the capture streak (vNext: capture-only streak)
            if (lastCaptureTime > 0 && (millis() - lastCaptureTime) > STREAK_TIMEOUT_MS) {
                captureStreak = 0;  // streak broken
            }
            captureStreak = (captureStreak < 255) ? captureStreak + 1 : 255;
            lastCaptureTime = millis();
            // Announce ultra streak at 20 captures
            if (captureStreak == 20 && !ultraStreakAnnounced) {
                Display::showToast("ULTRA STREAK!");
                SFX::play(SFX::ULTRA_STREAK);
                Avatar::triggerTailWiggle();
                ultraStreakAnnounced = true;
            }

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
        case XPEvent::SESSION_30MIN:
        case XPEvent::SESSION_60MIN:
        case XPEvent::SESSION_120MIN: {
            // Scale session time bonuses by level tier so they stay meaningful at high levels
            uint8_t lvl = data.cachedLevel;
            uint8_t tier = 0;
            if (lvl >= 41) tier = 4;
            else if (lvl >= 31) tier = 3;
            else if (lvl >= 21) tier = 2;
            else if (lvl >= 11) tier = 1;
            static const uint16_t SESSION_XP[5][3] = {
                { 10,  25,  50},   // L1-10  (base)
                { 15,  40,  80},   // L11-20
                { 25,  65, 130},   // L21-30
                { 40, 100, 200},   // L31-40
                { 60, 150, 300},   // L41-50
            };
            uint8_t col = (event == XPEvent::SESSION_30MIN) ? 0 :
                          (event == XPEvent::SESSION_60MIN) ? 1 : 2;
            amount = SESSION_XP[tier][col];
            break;
        }
        default:
            break;
    }

    // pig tracks your labor (challenges progress)
    Challenges::onXPEvent(event);
    
    // Apply capture XP multiplier for handshakes/PMKIDs (class buff: CR4CK_NOSE)
    if (event == XPEvent::HANDSHAKE_CAPTURED || event == XPEvent::PMKID_CAPTURED) {
        float captureMult = FlexesScreen::getCaptureXPMultiplier();
        amount = (uint16_t)(amount * captureMult);
        if (amount < 1) amount = 1;
    }
    
    // Apply distance XP multiplier for km walked (class buff: R04D_H0G)
    if (event == XPEvent::DISTANCE_KM) {
        float distMult = FlexesScreen::getDistanceXPMultiplier();
        amount = (uint16_t)(amount * distMult);
        if (amount < 1) amount = 1;
    }
    
    // Determine if streak bonus applies for this event (capture-only in vNext)
    bool isCaptureEvt = (event == XPEvent::HANDSHAKE_CAPTURED ||
                         event == XPEvent::PMKID_CAPTURED ||
                         event == XPEvent::DNH_PMKID_GHOST);
    streakAppliesForCurrentEvent = isCaptureEvt;
    addXP(amount);
    // Reset flag immediately after awarding XP to avoid affecting subsequent events
    streakAppliesForCurrentEvent = false;
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
            // Prevent overflow by checking before multiplication
            if (amount <= UINT16_MAX / 5) {
                amount *= 5;
            } else {
                amount = UINT16_MAX;  // Cap at max value
            }
            Display::showToast("JACKPOT!");
            SFX::play(SFX::JACKPOT_XP);  // Non-blocking celebration
            Avatar::triggerTailWiggle();
        } else if (roll >= 90) {
            // Bonus! 8% chance for 2x
            // Prevent overflow by checking before multiplication
            if (amount <= UINT16_MAX / 2) {
                amount *= 2;
            } else {
                amount = UINT16_MAX;  // Cap at max value
            }
        }
    }
    
    // ============ DOPAMINE HOOK: STREAK BONUS ============
    // Apply multiplier only if this event is capture-related (handshake/PMKID) and we have an active streak
    // vNext streak curve: 3–4 captures +10%, 5–9 +20%, 10–19 +35%, 20+ +50%
    if (streakAppliesForCurrentEvent) {
        if (captureStreak >= 20) {
            // 20 or more captures -> +50%
            // Prevent overflow: multiply by 1.5 without exceeding limits
            if (amount <= UINT16_MAX / 3) {
                amount = (uint16_t)((amount * 150) / 100);
            } else {
                amount = UINT16_MAX;  // Cap at max value
            }
        } else if (captureStreak >= 10) {
            // 10–19 captures -> +35%
            // Prevent overflow: multiply by 1.35 without exceeding limits
            if (amount <= UINT16_MAX / 1.35) {
                amount = (uint16_t)((amount * 135) / 100);
            } else {
                amount = UINT16_MAX;  // Cap at max value
            }
        } else if (captureStreak >= 5) {
            // 5–9 captures -> +20%
            // Prevent overflow: multiply by 1.2 without exceeding limits
            if (amount <= UINT16_MAX / 1.2) {
                amount = (uint16_t)((amount * 120) / 100);
            } else {
                amount = UINT16_MAX;  // Cap at max value
            }
        } else if (captureStreak >= 3) {
            // 3–4 captures -> +10%
            // Prevent overflow: multiply by 1.1 without exceeding limits
            if (amount <= UINT16_MAX / 1.1) {
                amount = (uint16_t)((amount * 110) / 100);
            } else {
                amount = UINT16_MAX;  // Cap at max value
            }
        }
    }
    
    // Apply buff/debuff XP multiplier (SNOUT$HARP +18%, F0GSNOUT -10%)
    float mult = FlexesScreen::getXPMultiplier();
    // Prevent overflow when applying multiplier
    uint32_t tempAmount = (uint32_t)amount * mult;
    uint16_t modifiedAmount = (tempAmount > UINT16_MAX) ? UINT16_MAX : (uint16_t)tempAmount;
    if (modifiedAmount < 1) modifiedAmount = 1;  // Always at least 1 XP
    
    uint8_t oldLevel = data.cachedLevel;
    
    // Prevent total XP overflow - cap at max possible level threshold
    uint32_t maxThreshold = getXPForLevel(MAX_LEVEL);
    if (data.totalXP <= maxThreshold && (data.totalXP + modifiedAmount) > maxThreshold) {
        // Cap at max level threshold to prevent overflow beyond max level
        modifiedAmount = (uint32_t)(maxThreshold - data.totalXP);
    } else if (data.totalXP > maxThreshold) {
        // If already past max level, don't add more XP
        modifiedAmount = 0;
    }
    
    if (modifiedAmount > 0) {
        data.totalXP += modifiedAmount;
        session.xp += modifiedAmount;

        // Record last significant XP gain for UI (show +XP<N> under the bar)
        if (modifiedAmount > 2) {
            lastXPGainAmount = modifiedAmount;
            lastXPGainMs = millis();
        }
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

void XP::addXPSilent(uint16_t amount) {
    // Silent XP add - no JACKPOT check, no toast, no sound
    // Used for bonus XP from challenges/achievements where celebration already shown
    
    // Apply buff/debuff XP multiplier (SNOUT$HARP +18%, F0GSNOUT -10%)
    float mult = FlexesScreen::getXPMultiplier();
    // Prevent overflow when applying multiplier
    uint32_t tempAmount = (uint32_t)amount * mult;
    uint16_t modifiedAmount = (tempAmount > UINT16_MAX) ? UINT16_MAX : (uint16_t)tempAmount;
    if (modifiedAmount < 1) modifiedAmount = 1;
    
    uint8_t oldLevel = data.cachedLevel;
    
    // Prevent total XP overflow - cap at max possible level threshold
    uint32_t maxThreshold = getXPForLevel(MAX_LEVEL);
    if (data.totalXP <= maxThreshold && (data.totalXP + modifiedAmount) > maxThreshold) {
        // Cap at max level threshold to prevent overflow beyond max level
        modifiedAmount = (uint32_t)(maxThreshold - data.totalXP);
    } else if (data.totalXP > maxThreshold) {
        // If already past max level, don't add more XP
        modifiedAmount = 0;
    }
    
    if (modifiedAmount > 0) {
        data.totalXP += modifiedAmount;
        session.xp += modifiedAmount;

        // Record last significant XP gain for UI
        if (modifiedAmount > 2) {
            lastXPGainAmount = modifiedAmount;
            lastXPGainMs = millis();
        }
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

void XP::addRouletteWin() {
    data.rouletteWins++;
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
    // Return bonus: deferred until clock is valid (GPS/NTP sync)
    if (!returnBonusChecked) {
        time_t now = time(nullptr);
        if (now > 1700000000) {
            returnBonusChecked = true;
            if (lastSessionEpoch > 0 && (uint32_t)now > lastSessionEpoch) {
                uint32_t elapsed = (uint32_t)now - lastSessionEpoch;
                if (elapsed >= 172800) {  // 48 hours in seconds
                    uint16_t bonus = 25 + (data.cachedLevel * 2);
                    addXPSilent(bonus);
                    Display::showToast("PIG MISSED YOU.");
                    SFX::play(SFX::ACHIEVEMENT);
                    Serial.printf("[XP] Return bonus: +%d XP (absent %lu hours)\n",
                                  bonus, (unsigned long)(elapsed / 3600));
                }
            }
        }
    }

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
    for (uint8_t i = MAX_LEVEL - 1; i > 0; i--) {
        if (xp >= XP_THRESHOLDS[i]) {
            return i + 1;  // Levels are 1-indexed
        }
    }
    return 1;
}

uint32_t XP::getXPForLevel(uint8_t level) {
    if (level <= 1) return 0;
    if (level > MAX_LEVEL) level = MAX_LEVEL;
    return XP_THRESHOLDS[level - 1];
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
    if (level >= 46) return PorkClass::B4C0NM4NC3R;
    if (level >= 41) return PorkClass::K3RN3L_H0G;
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
    if (idx > 9) idx = 9;
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
    
    // Queue achievement for celebration (prevents cascade of sounds)
    // Celebration happens in processAchievementQueue() called from main loop
    if (initialized) {
        if (achQueueMutex != nullptr && xSemaphoreTake(achQueueMutex, portMAX_DELAY) == pdTRUE) {
            uint8_t nextHead = (achQueueHead + 1) % ACH_QUEUE_SIZE;
            if (nextHead != achQueueTail) {  // Not full
                achQueue[achQueueHead] = ach;
                achQueueHead = nextHead;
            }
            pendingSaveFlag = true;
            xSemaphoreGive(achQueueMutex);
        }
    } else {
        pendingSaveFlag = true;  // pre-init path, no mutex needed yet
    }
}

void XP::processAchievementQueue() {
    // Process ONE queued achievement per call (debounced)
    // Called from main loop to spread celebrations across frames
    if (achQueueMutex != nullptr && xSemaphoreTake(achQueueMutex, portMAX_DELAY) == pdTRUE) {
        if (achQueueTail == achQueueHead) {
            xSemaphoreGive(achQueueMutex);
            return;  // Queue empty
        }

        uint32_t now = millis();
        if (now - lastAchievementTime < ACH_COOLDOWN_MS) {
            xSemaphoreGive(achQueueMutex);
            return;  // Cooldown active
        }

        PorkAchievement ach = achQueue[achQueueTail];
        achQueueTail = (achQueueTail + 1) % ACH_QUEUE_SIZE;
        lastAchievementTime = now;
        xSemaphoreGive(achQueueMutex);

        // Find achievement name
        uint8_t idx = 0;
        uint64_t mask = 1ULL;
        while (mask < (uint64_t)ach && idx < ACHIEVEMENT_COUNT - 1) {
            mask <<= 1;
            idx++;
        }

        // NOW do the celebration (one at a time, with cooldown)
        char toastMsg[48];
        snprintf(toastMsg, sizeof(toastMsg), "* %s *", ACHIEVEMENT_NAMES[idx]);
        Display::showToast(toastMsg);
        SFX::play(SFX::ACHIEVEMENT);
        Avatar::triggerTailWiggle();
    }
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

uint8_t XP::getAchievementCount() {
    return ACHIEVEMENT_COUNT;
}

// Unlockables (v0.1.8) - secret challenges
void XP::setUnlockable(uint8_t bitIndex) {
    if (bitIndex >= 32) return;  // Only 32 bits available
    if (achQueueMutex != nullptr && xSemaphoreTake(achQueueMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        data.unlockables |= (1UL << bitIndex);
        pendingSaveFlag = true;
        xSemaphoreGive(achQueueMutex);
    } else {
        data.unlockables |= (1UL << bitIndex);  // best-effort if mutex unavailable
        pendingSaveFlag = true;
    }
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
    if (data.cachedLevel >= 50 && !hasAchievement(ACH_MAX_LEVEL)) {
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
    
    // 50 bros added lifetime = Full Roster
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
    // Draw XP bar at TOP of main canvas (y=0) with inverted colors for visibility
    // Format: "L## TITLE_FULL      ######.......... 100%"
    // Progress bar and percentage aligned to right edge
    int barY = 1;
    int barH = 10;  // Height of inverted background strip
    
    // Draw inverted background strip
    canvas.fillRect(0, 0, DISPLAY_W, barH, COLOR_FG);
    
    canvas.setTextSize(1);
    canvas.setTextColor(COLOR_BG);  // Inverted text color
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
    
    // Title - fill space between level and XP: label (avoid String to prevent heap churn)
    const char* title = getTitle();
    int titleX = 2 + levelW + 4;  // 4px gap after level
    int maxTitleW = xpLabelX - titleX - 4;  // Available space for title
    
    // Check if title fits without truncation
    int titleW = canvas.textWidth(title);
    if (titleW <= maxTitleW) {
        // Title fits, draw as-is
        canvas.drawString(title, titleX, barY);
    } else {
        // Title too long, truncate to stack buffer to avoid heap alloc
        char titleBuf[24];  // Max title length ~15 chars + ".." + null
        size_t titleLen = strlen(title);
        if (titleLen > sizeof(titleBuf) - 3) titleLen = sizeof(titleBuf) - 3;
        
        // Binary search for fitting length
        size_t len = titleLen;
        while (len > 3) {
            memcpy(titleBuf, title, len);
            titleBuf[len] = '\0';
            if (canvas.textWidth(titleBuf) + canvas.textWidth("..") <= maxTitleW) {
                break;
            }
            len--;
        }
        
        // Append ".." if truncated
        if (len < titleLen) {
            strcpy(titleBuf + len, "..");
        }
        canvas.drawString(titleBuf, titleX, barY);
    }
}

// ============ TOP BAR XP NOTIFICATION (Option B) ============
// Shows inverted XP info in top bar for 5 seconds after XP gain

static const uint32_t XP_TOPBAR_DISPLAY_MS = 5000;  // 5 seconds

bool XP::shouldShowXPNotification() {
    // Show notification for 5 seconds after any XP gain > 2
    return (lastXPGainAmount > 2 && (millis() - lastXPGainMs) < XP_TOPBAR_DISPLAY_MS);
}

uint16_t XP::getLastXPGainAmount() {
    return lastXPGainAmount;
}

void XP::drawTopBarXP(M5Canvas& topBar) {
    // Draw inverted XP bar in top bar (replaces normal top bar content)
    // Format: "L## TITLE +XX XP!" or "L## TITLE ####.... %%"
    
    // Invert top bar
    topBar.fillSprite(COLOR_FG);
    topBar.setTextColor(COLOR_BG);
    topBar.setTextSize(1);
    topBar.setTextDatum(top_left);
    
    // Build level string
    char levelStr[8];
    snprintf(levelStr, sizeof(levelStr), "L%d", getLevel());
    
    // Get title
    const char* title = getTitle();
    
    // Build XP gain string with progress
    uint8_t progress = getProgress();
    char xpStr[32];
    snprintf(xpStr, sizeof(xpStr), "+%u XP (%d%%)", (unsigned)lastXPGainAmount, progress);
    
    // Calculate positions
    int levelW = topBar.textWidth(levelStr);
    int titleX = 2 + levelW + 4;
    int xpW = topBar.textWidth(xpStr);
    int xpX = DISPLAY_W - xpW - 2;
    
    // Max title width (avoid String to prevent heap churn in hot path)
    int maxTitleW = xpX - titleX - 6;
    int titleW = topBar.textWidth(title);
    
    if (titleW <= maxTitleW) {
        // Title fits, draw as-is
        topBar.drawString(title, titleX, 3);
    } else {
        // Title too long, truncate to stack buffer
        char titleBuf[24];
        size_t titleLen = strlen(title);
        if (titleLen > sizeof(titleBuf) - 3) titleLen = sizeof(titleBuf) - 3;
        
        // Find fitting length
        size_t len = titleLen;
        while (len > 3) {
            memcpy(titleBuf, title, len);
            titleBuf[len] = '\0';
            if (topBar.textWidth(titleBuf) + topBar.textWidth("..") <= maxTitleW) {
                break;
            }
            len--;
        }
        
        // Append ".." if truncated
        if (len < titleLen) {
            strcpy(titleBuf + len, "..");
        }
        topBar.drawString(titleBuf, titleX, 3);
    }
    
    // Draw level and XP gain
    topBar.drawString(levelStr, 2, 3);
    topBar.setTextDatum(top_right);
    topBar.drawString(xpStr, DISPLAY_W - 2, 3);
}
