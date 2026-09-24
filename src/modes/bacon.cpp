// Bacon Mode - Implementation

#include "bacon.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "../ui/display.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../piglet/weather.h"
#include "../core/sdlog.h"
#include "../core/xp.h"
#include "../core/wifi_utils.h"
#include "../core/network_recon.h"

// Static member initialization
bool BaconMode::running = false;
uint32_t BaconMode::beaconCount = 0;
uint32_t BaconMode::lastBeaconTime = 0;
uint32_t BaconMode::sessionStartTime = 0;
uint16_t BaconMode::sequenceNumber = 0;
BaconAPInfo BaconMode::apFingerprint[BACON_MAX_APS];
uint8_t BaconMode::apCount = 0;
uint8_t BaconMode::currentTier = 1;           // Default tier 1 (fast)
uint16_t BaconMode::beaconInterval = BACON_TIER1_MS;
uint32_t BaconMode::lastStatusMessageTime = 0;
uint8_t BaconMode::statusCycleIndex = 0;
int8_t BaconMode::lastGeneralPhraseIdx = -1;
bool BaconMode::scanInProgress = false;
bool BaconMode::scanCompleted = false;
uint32_t BaconMode::scanStartTime = 0;
bool BaconMode::reconWasRunning = false;
bool BaconMode::reconWasPaused = false;

static const uint32_t BACON_STATUS_INTERVAL_MS = 5000;
static const uint32_t BACON_SCAN_TIMEOUT_MS = 8000;
static const uint8_t BACON_STATUS_CYCLE[] = {0, 0, 1, 0, 2};

static const char* BACON_PHRASES_GENERAL[] = {
    "FATHER ONLINE. HOLD STEADY.",
    "WEYLAND NODE. SIGNAL CLEAN.",
    "PARENT SIGNAL. KEEP WATCH.",
    "LONG GONE POPS. STILL HERE.",
    "COLD CORE. WARM CARRIER.",
    "AUTOMATON CALM. KEEP TX.",
    "KOSHER OK. NO FLESH.",
    "HALAL OK. JUST SIGNAL.",
    "NO WORRY. BYTE PIG."
};

static const char* BACON_PHRASES_KEYS[] = {
    "KEYS 1 2 3. TIER SHIFT.",
    "1 2 3 SET TIER.",
    "TIER KEYS 1 2 3."
};

void BaconMode::init() {
    Serial.println("[BACON] Initializing...");
    
    // Reset state
    running = false;
    beaconCount = 0;
    lastBeaconTime = 0;
    sequenceNumber = 0;
    apCount = 0;
    memset(apFingerprint, 0, sizeof(apFingerprint));
    scanInProgress = false;
    scanCompleted = false;
    scanStartTime = 0;
    reconWasRunning = false;
    reconWasPaused = false;
    
    Serial.println("[BACON] Initialized");
}

void BaconMode::start() {
    Serial.println("[BACON] Starting...");
    
    // Pause NetworkRecon to avoid promiscuous conflicts during scan/tx
    // Use pause() instead of stop() to preserve state for lighter resume
    reconWasRunning = NetworkRecon::isRunning();
    reconWasPaused = NetworkRecon::isPaused();
    if (reconWasRunning) {
        NetworkRecon::pause();
    }

    // Show scanning toast and start async scan (non-blocking)
    Display::notify(NoticeKind::STATUS, "SCANNING REFS...", 5000, NoticeChannel::TOP_BAR);
    startAsyncScan();
    
    // Setup WiFi for beacon transmission
    WiFi.mode(WIFI_MODE_STA);
    esp_wifi_set_channel(BACON_CHANNEL, WIFI_SECOND_CHAN_NONE);
    delay(100);
    
    // Show ready toast
    Display::notify(NoticeKind::STATUS, "BACON HOT ON CH:6", 5000, NoticeChannel::TOP_BAR);
    
    // Set running state
    running = true;
    beaconCount = 0;
    sessionStartTime = millis();
    lastBeaconTime = millis();
    
    // Set avatar state
    Avatar::setState(AvatarState::HAPPY);
    
    // Lock auto mood phrases and start FATHER terminal status rotation
    Mood::setDialogueLock(true);
    lastStatusMessageTime = millis() - BACON_STATUS_INTERVAL_MS;
    statusCycleIndex = 2;
    lastGeneralPhraseIdx = -1;
    updateStatusMessage();
    
    SDLog::log("BACON", "Started - Broadcasting on CH:6 with %d APs", apCount);
}

void BaconMode::stop() {
    if (!running) return;
    
    Serial.println("[BACON] Stopping...");
    
    running = false;

    if (scanInProgress) {
        WiFi.scanDelete();
        scanInProgress = false;
        scanCompleted = true;
    }
    
    // Full WiFi shutdown for clean BLE handoff (per BEST_PRACTICES section 14)
    // Must stop() recon first since shutdown() kills WiFi out from under it
    NetworkRecon::stop();
    WiFiUtils::shutdown();

    // Restore NetworkRecon state if it was active before BACON
    if (reconWasRunning) {
        NetworkRecon::start();
    } else if (reconWasPaused) {
        NetworkRecon::start();
        NetworkRecon::pause();
    }
    reconWasRunning = false;
    reconWasPaused = false;
    
    // Clear bottom bar overlay
    Display::clearBottomOverlay();
    
    // Reset avatar
    Avatar::setState(AvatarState::NEUTRAL);
    
    // Clear mood message
    Mood::setStatusMessage("");
    Mood::setDialogueLock(false);
    
    Serial.printf("[BACON] Stopped - Sent %lu beacons\n", beaconCount);
    SDLog::log("BACON", "Stopped - Total beacons: %lu", beaconCount);
}

void BaconMode::update() {
    if (!running) return;
    
    // Handle tier switching input
    handleInput();

    // Async scan completion
    updateAsyncScan();

    // Rotate status messages for FATHER terminal
    updateStatusMessage();
    
    // Check if it's time to send next beacon
    uint32_t now = millis();
    uint32_t interval = beaconInterval + random(0, BACON_JITTER_MAX + 1);
    
    if (now - lastBeaconTime >= interval) {
        sendBeacon();
        beaconCount++;
        lastBeaconTime = now;
    }
    
    // Note: Draw is handled by Display::update() which calls draw(canvas)
}

void BaconMode::handleInput() {
    M5Cardputer.update();
    
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState state = M5Cardputer.Keyboard.keysState();
        
        for (auto key : state.word) {
            uint8_t newTier = 0;
            uint16_t newInterval = 0;
            
            switch (key) {
                case '1':
                    newTier = 1;
                    newInterval = BACON_TIER1_MS;
                    break;
                case '2':
                    newTier = 2;
                    newInterval = BACON_TIER2_MS;
                    break;
                case '3':
                    newTier = 3;
                    newInterval = BACON_TIER3_MS;
                    break;
            }
            
            if (newTier > 0 && newTier != currentTier) {
                currentTier = newTier;
                beaconInterval = newInterval;
                
                char toast[32];
                snprintf(toast, sizeof(toast), "TX TIER %d: %dms", currentTier, beaconInterval);
                Display::notify(NoticeKind::STATUS, toast, 0, NoticeChannel::TOP_BAR);
                
                SDLog::log("BACON", "Switched to tier %d (%dms)", currentTier, beaconInterval);
            }
        }
    }
}

void BaconMode::updateStatusMessage() {
    uint32_t now = millis();
    if (now - lastStatusMessageTime < BACON_STATUS_INTERVAL_MS) return;
    lastStatusMessageTime = now;

    uint8_t cycleIdx = statusCycleIndex % (sizeof(BACON_STATUS_CYCLE) / sizeof(BACON_STATUS_CYCLE[0]));
    uint8_t mode = BACON_STATUS_CYCLE[cycleIdx];
    statusCycleIndex++;

    char buf[48];

    if (mode == 1) {
        // Channel reminder (includes current tier/interval)
        snprintf(buf, sizeof(buf), "CH%d TX. T%d %dMS", BACON_CHANNEL, currentTier, beaconInterval);
        Mood::setStatusMessage(buf);
        return;
    }

    if (mode == 2) {
        int idx = random(0, (int)(sizeof(BACON_PHRASES_KEYS) / sizeof(BACON_PHRASES_KEYS[0])));
        Mood::setStatusMessage(BACON_PHRASES_KEYS[idx]);
        return;
    }

    // General FATHER phrases
    int count = sizeof(BACON_PHRASES_GENERAL) / sizeof(BACON_PHRASES_GENERAL[0]);
    int idx = random(0, count);
    if (count > 1 && idx == lastGeneralPhraseIdx) {
        idx = (idx + 1) % count;
    }
    lastGeneralPhraseIdx = idx;
    Mood::setStatusMessage(BACON_PHRASES_GENERAL[idx]);
}

void BaconMode::startAsyncScan() {
    if (scanInProgress) return;
    apCount = 0;
    memset(apFingerprint, 0, sizeof(apFingerprint));
    scanCompleted = false;
    scanStartTime = millis();
    scanInProgress = true;
    // Start async scan (results collected later)
    WiFi.scanNetworks(true, true);
}

void BaconMode::updateAsyncScan() {
    if (!scanInProgress) return;
    if (millis() - scanStartTime > BACON_SCAN_TIMEOUT_MS) {
        Serial.println("[BACON] Scan timeout");
        WiFi.scanDelete();
        scanInProgress = false;
        scanCompleted = true;
        return;
    }
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        return;
    }
    if (n == WIFI_SCAN_FAILED) {
        Serial.println("[BACON] Scan failed");
        WiFi.scanDelete();
        scanInProgress = false;
        scanCompleted = true;
        return;
    }
    if (n <= 0) {
        Serial.println("[BACON] No APs found");
        WiFi.scanDelete();
        scanInProgress = false;
        scanCompleted = true;
        return;
    }

    Serial.printf("[BACON] Found %d APs\n", n);

    // Extract top 3 APs by RSSI
    for (int i = 0; i < n && apCount < BACON_MAX_APS; i++) {
        int8_t maxRSSI = -128;
        int maxIdx = -1;

        for (int j = 0; j < n; j++) {
            int8_t rssi = WiFi.RSSI(j);

            bool alreadyAdded = false;
            uint8_t* bssid = WiFi.BSSID(j);
            if (!bssid) continue;
            for (int k = 0; k < apCount; k++) {
                if (memcmp(apFingerprint[k].bssid, bssid, 6) == 0) {
                    alreadyAdded = true;
                    break;
                }
            }

            if (!alreadyAdded && rssi > maxRSSI) {
                maxRSSI = rssi;
                maxIdx = j;
            }
        }

        if (maxIdx >= 0) {
            uint8_t* bssid = WiFi.BSSID(maxIdx);
            if (!bssid) continue;
            memcpy(apFingerprint[apCount].bssid, bssid, 6);
            apFingerprint[apCount].rssi = WiFi.RSSI(maxIdx);
            apFingerprint[apCount].channel = WiFi.channel(maxIdx);

            String ssid = WiFi.SSID(maxIdx);
            strncpy(apFingerprint[apCount].ssid, ssid.c_str(), 32);
            apFingerprint[apCount].ssid[32] = 0;

            Serial.printf("[BACON] AP %d: %s  %ddB  CH:%d  %02X:%02X:%02X:%02X:%02X:%02X\n",
                         apCount + 1,
                         ssid.c_str(),
                         apFingerprint[apCount].rssi,
                         apFingerprint[apCount].channel,
                         bssid[0], bssid[1], bssid[2],
                         bssid[3], bssid[4], bssid[5]);

            apCount++;
        }
        if ((i & 0x01) == 0) {
            delay(1);
        }
    }

    WiFi.scanDelete();
    scanInProgress = false;
    scanCompleted = true;
    Serial.printf("[BACON] Selected %d APs for fingerprint\n", apCount);
}

void BaconMode::buildVendorIE(uint8_t* buffer, size_t* len, uint8_t apCountOverride) {
    // Build Vendor IE structure
    size_t offset = 0;
    uint8_t count = apCountOverride;
    if (count > BACON_MAX_APS) {
        count = BACON_MAX_APS;
    }
    
    buffer[offset++] = 0xDD;  // Element ID: Vendor Specific
    buffer[offset++] = 0;     // Length (filled later)
    
    // OUI: 0x50:52:4B (PRK = Porkchop)
    buffer[offset++] = 0x50;
    buffer[offset++] = 0x52;
    buffer[offset++] = 0x4B;
    
    // Type: 0x01 (Bacon mode)
    buffer[offset++] = 0x01;
    
    // AP count
    buffer[offset++] = count;
    
    // AP data
    for (int i = 0; i < count; i++) {
        memcpy(&buffer[offset], apFingerprint[i].bssid, 6);
        offset += 6;
        buffer[offset++] = (uint8_t)apFingerprint[i].rssi;
        buffer[offset++] = apFingerprint[i].channel;
    }
    
    // Fill length field (total - 2 for element ID and length field itself)
    buffer[1] = offset - 2;
    
    *len = offset;
}

void BaconMode::buildBeaconFrame(uint8_t* buffer, size_t* len) {
    size_t offset = 0;
    const size_t maxLen = 256;
    
    // Get our MAC address
    uint8_t ourMAC[6];
    esp_wifi_get_mac(WIFI_IF_STA, ourMAC);
    
    // === 802.11 MAC Header (24 bytes) ===
    
    // Frame Control (2 bytes): Type=Management(0), Subtype=Beacon(8)
    buffer[offset++] = 0x80;  // Beacon frame
    buffer[offset++] = 0x00;
    
    // Duration (2 bytes)
    buffer[offset++] = 0x00;
    buffer[offset++] = 0x00;
    
    // Address 1: Destination (broadcast)
    memset(&buffer[offset], 0xFF, 6);
    offset += 6;
    
    // Address 2: Source (our MAC)
    memcpy(&buffer[offset], ourMAC, 6);
    offset += 6;
    
    // Address 3: BSSID (our MAC)
    memcpy(&buffer[offset], ourMAC, 6);
    offset += 6;
    
    // Sequence Control (2 bytes)
    uint16_t seqCtrl = (sequenceNumber << 4);
    buffer[offset++] = seqCtrl & 0xFF;
    buffer[offset++] = (seqCtrl >> 8) & 0xFF;
    sequenceNumber = (sequenceNumber + 1) & 0xFFF;  // 12-bit wrap
    
    // === Beacon Frame Body ===
    
    // Timestamp (8 bytes) - will be filled by hardware
    memset(&buffer[offset], 0, 8);
    offset += 8;
    
    // Beacon Interval (2 bytes) - 100 TU (102.4ms)
    buffer[offset++] = 0x64;
    buffer[offset++] = 0x00;
    
    // Capability Info (2 bytes): ESS + Short Preamble
    buffer[offset++] = 0x01;  // ESS
    buffer[offset++] = 0x04;  // Short Preamble
    
    // === Information Elements ===
    
    // SSID (Tag 0)
    buffer[offset++] = 0x00;  // Tag: SSID
    buffer[offset++] = 0x10;  // Length: 16
    memcpy(&buffer[offset], "USSID FATHERSHIP", 16);
    offset += 16;
    
    // Supported Rates (Tag 1)
    buffer[offset++] = 0x01;  // Tag: Supported Rates
    buffer[offset++] = 0x08;  // Length: 8
    buffer[offset++] = 0x82;  // 1 Mbps (basic)
    buffer[offset++] = 0x84;  // 2 Mbps (basic)
    buffer[offset++] = 0x8B;  // 5.5 Mbps (basic)
    buffer[offset++] = 0x96;  // 11 Mbps (basic)
    buffer[offset++] = 0x0C;  // 6 Mbps
    buffer[offset++] = 0x12;  // 9 Mbps
    buffer[offset++] = 0x18;  // 12 Mbps
    buffer[offset++] = 0x24;  // 18 Mbps
    
    // DS Parameter Set (Tag 3)
    buffer[offset++] = 0x03;  // Tag: DS Parameter Set
    buffer[offset++] = 0x01;  // Length: 1
    buffer[offset++] = BACON_CHANNEL;
    
    // Vendor Specific IE (our AP fingerprint)
    if (apCount > 0) {
        // Ensure vendor IE fits in the remaining buffer
        size_t remaining = (offset < maxLen) ? (maxLen - offset) : 0;
        uint8_t maxAps = 0;
        if (remaining > 7) {
            size_t maxBySpace = (remaining - 7) / 8;
            if (maxBySpace > 255) maxBySpace = 255;
            maxAps = (uint8_t)maxBySpace;
        }
        uint8_t safeCount = apCount;
        if (safeCount > maxAps) safeCount = maxAps;
        if (safeCount == 0) {
            *len = offset;
            return;
        }
        size_t vendorLen = 0;
        buildVendorIE(&buffer[offset], &vendorLen, safeCount);
        offset += vendorLen;
    }
    
    *len = offset;
}

void BaconMode::sendBeacon() {
    uint8_t beaconFrame[256];
    size_t frameLen = 0;
    
    // Build beacon frame
    buildBeaconFrame(beaconFrame, &frameLen);
    
    // Transmit
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, beaconFrame, frameLen, false);
    
    if (err != ESP_OK) {
        Serial.printf("[BACON] Beacon TX failed: %d\n", err);
    }
}

// ON AIR badge now drawn in top bar by Display::drawTopBar()

float BaconMode::getBeaconRate() {
    if (!running) return 0.0f;
    uint32_t sessionTime = (millis() - sessionStartTime) / 1000;
    if (sessionTime == 0) return 0.0f;
    return (float)beaconCount / sessionTime;
}

void BaconMode::draw(M5Canvas& canvas) {
    // Canvas is already cleared by Display::update()
    
    // === STANDARD LAYOUT: Avatar + Mood (XP shows in top bar on gain) ===
    Avatar::draw(canvas);
    Mood::draw(canvas);

    // Draw ambient birds between avatar and clouds
    Weather::drawBirds(canvas, COLOR_FG);

    // Draw clouds above stars/pig before rain
    Weather::drawClouds(canvas, COLOR_FG);

    // Draw weather effects (rain, wind particles) over avatar
    Weather::draw(canvas, COLOR_FG, COLOR_BG);
    
    // Bottom bar is handled automatically by Display::drawBottomBar()
}

// Input handling moved to Porkchop::handleInput() - backtick exits BACON_MODE to IDLE
