// DO NO HAM Mode implementation
// "BRAVO 6, GOING DARK"
// Passive WiFi reconnaissance - no attacks, just listening

#include "donoham.h"
#include <M5Unified.h>
#include <WiFi.h>
#include <NimBLEDevice.h>  // For BLE coexistence check
#include "../core/config.h"
#include "../core/sd_layout.h"
#include "../audio/sfx.h"
#include "../core/sdlog.h"
#include "../core/xp.h"
#include "../core/wsl_bypasser.h"
#include "../core/wifi_utils.h"
#include "../core/heap_gates.h"
#include "../core/heap_policy.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include <SD.h>
#include <esp_heap_caps.h>

// PCAP file format structures (same as OINK for WPA-SEC compatibility)
#pragma pack(push, 1)
struct DNH_PCAPHeader {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t linktype;
};

struct DNH_PCAPPacketHeader {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
};
#pragma pack(pop)

// Minimal radiotap header (8 bytes) - required for WPA-SEC
static const uint8_t DNH_RADIOTAP_HEADER[] = {
    0x00,       // Header revision
    0x00,       // Header pad
    0x08, 0x00, // Header length (8, little-endian)
    0x00, 0x00, 0x00, 0x00  // Present flags (no optional fields)
};

// Static member initialization
bool DoNoHamMode::running = false;
DNHState DoNoHamMode::state = DNHState::HOPPING;
uint8_t DoNoHamMode::currentChannel = 1;
uint8_t DoNoHamMode::channelIndex = 0;
uint32_t DoNoHamMode::dwellStartTime = 0;
bool DoNoHamMode::dwellResolved = false;

// networks vector moved to NetworkRecon - use networks() helper below
std::vector<CapturedPMKID> DoNoHamMode::pmkids;
std::vector<CapturedHandshake> DoNoHamMode::handshakes;

// networks reference - now uses shared NetworkRecon vector
static inline std::vector<DetectedNetwork>& networks() {
    return NetworkRecon::getNetworks();
}

// Adaptive state machine
ChannelStats DoNoHamMode::channelStats[13] = {};
std::vector<IncompleteHS> DoNoHamMode::incompleteHandshakes;
uint32_t DoNoHamMode::huntStartTime = 0;
uint32_t DoNoHamMode::lastHuntTime = 0;
uint8_t DoNoHamMode::lastHuntChannel = 0;
uint32_t DoNoHamMode::lastStatsDecay = 0;
uint8_t DoNoHamMode::lastCycleActivity = 0;
uint32_t DoNoHamMode::adaptiveDwellUntil = 0;

// Guard flag for race condition prevention
static volatile bool dnhBusy = false;

// Protect pending handshake payload from partial writes in callback
static portMUX_TYPE pendingHandshakeMux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE pendingPMKIDMux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE pendingBeaconMux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE pendingIncompleteMux = portMUX_INITIALIZER_UNLOCKED;

// Ring-buffered deferred PMKID create (small, static)
static const uint8_t PENDING_PMKID_SLOTS = 4;
static uint8_t pendingPMKIDWrite = 0;
static uint8_t pendingPMKIDRead = 0;
static uint8_t pendingPMKIDCount = 0;
struct PendingPMKIDCreate {
    uint8_t bssid[6];
    uint8_t station[6];
    uint8_t pmkid[16];
    char ssid[33];
    uint8_t channel;
};
static PendingPMKIDCreate pendingPMKIDRing[PENDING_PMKID_SLOTS];

static void onNewNetworkDiscovered(wifi_auth_mode_t authmode, bool isHidden,
                                   const char* ssid, int8_t rssi, uint8_t channel) {
    (void)authmode;
    (void)isHidden;
    (void)ssid;
    (void)rssi;
    (void)channel;
    XP::addXP(XPEvent::DNH_NETWORK_PASSIVE);
}

struct PendingHandshakeFrame {
    uint8_t bssid[6];
    uint8_t station[6];
    uint8_t messageNum;  // DEPRECATED - kept for compatibility
    EAPOLFrame frames[4];  // Store all 4 EAPOL frames (M1-M4)
    uint8_t capturedMask;  // Bitmask: bit0=M1, bit1=M2, bit2=M3, bit3=M4
};
// Ring-buffered deferred handshake frame add (heap allocated on start)
static const uint8_t PENDING_HS_SLOTS = 2;
static PendingHandshakeFrame pendingHandshakeFallback;
static PendingHandshakeFrame* pendingHandshakePool = &pendingHandshakeFallback;
static bool pendingHandshakePoolAllocated = false;
static uint8_t pendingHandshakeSlots = 1;
static uint8_t pendingHandshakeWrite = 0;
static bool pendingHandshakeUsed[PENDING_HS_SLOTS] = {false, false};

// Handshake capture event for UI
static volatile bool pendingHandshakeCapture = false;
static char pendingHandshakeSSID[33] = {0};

// Deferred save flag - set during update(), processed with short recon pauses
// Avoids SD/WiFi SPI bus contention that can cause crashes
static volatile bool pendingSaveFlag = false;

// Deferred beacon storage for handshakes (ESP32 dual-core race: avoid malloc in callback)
// Callback copies beacon data here, update() does the malloc and attachment
static volatile bool pendingBeaconStore = false;
static uint8_t pendingBeaconBSSID[6];
static uint8_t pendingBeaconData[512];  // Static buffer, no malloc in callback
static uint16_t pendingBeaconLen = 0;

// Ring-buffered deferred incomplete handshake tracking (small, static)
static const uint8_t PENDING_INCOMPLETE_SLOTS = 8;
static uint8_t pendingIncompleteWrite = 0;
static uint8_t pendingIncompleteRead = 0;
static uint8_t pendingIncompleteCount = 0;
static IncompleteHS pendingIncompleteRing[PENDING_INCOMPLETE_SLOTS];
// Minimum free heap to allow new handshake allocations (handshake struct is large)
static const size_t DNH_HANDSHAKE_ALLOC_MIN_BLOCK = sizeof(CapturedHandshake) + HeapPolicy::kHandshakeAllocSlack;
static const size_t DNH_PMKID_ALLOC_MIN_BLOCK = sizeof(CapturedPMKID) + HeapPolicy::kPmkidAllocSlack;

// Channel order: 1, 6, 11 first (non-overlapping), then fill in
// Keep in sync with NetworkRecon hop order for consistent stats.
static const uint8_t CHANNEL_ORDER[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};

static int channelToIndex(uint8_t ch) {
    for (int i = 0; i < 13; i++) {
        if (CHANNEL_ORDER[i] == ch) {
            return i;
        }
    }
    return -1;
}

// Timing
static uint32_t lastCleanupTime = 0;
static uint32_t lastSaveTime = 0;
static uint32_t lastMoodTime = 0;

void DoNoHamMode::init() {
}

void DoNoHamMode::start() {
    if (running) return;
    
    Serial.println("[DNH] Starting passive mode...");
    SDLog::log("DNH", "Starting passive mode");
    
    // Ensure NetworkRecon is running (handles WiFi promiscuous mode)
    if (!NetworkRecon::isRunning()) {
        NetworkRecon::start();
    }
    
    // Clear DNH-specific data (networks is shared via NetworkRecon)
    pmkids.clear();
    pmkids.shrink_to_fit();
    handshakes.clear();
    handshakes.shrink_to_fit();
    incompleteHandshakes.clear();
    incompleteHandshakes.shrink_to_fit();

    // Reserve memory for captures
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest >= (sizeof(CapturedPMKID) * 8 + HeapPolicy::kReserveSlackSmall)) {
        pmkids.reserve(8);
    }
    largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest >= (sizeof(CapturedHandshake) * 4 + HeapPolicy::kReserveSlackLarge)) {
        handshakes.reserve(4);
    }
    largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest >= (sizeof(IncompleteHS) * 8 + HeapPolicy::kReserveSlackSmall)) {
        incompleteHandshakes.reserve(8);
    }
    
    // Initialize channel stats
    for (int i = 0; i < 13; i++) {
        channelStats[i].channel = CHANNEL_ORDER[i];
        channelStats[i].beaconCount = 0;
        channelStats[i].eapolCount = 0;
        channelStats[i].lastActivity = 0;
        channelStats[i].priority = 100;  // Baseline
        channelStats[i].deadStreak = 0;
        channelStats[i].lifetimeBeacons = 0;
    }
    
    // Reset state
    state = DNHState::HOPPING;
    currentChannel = NetworkRecon::getCurrentChannel();
    int startIdx = channelToIndex(currentChannel);
    channelIndex = (startIdx >= 0) ? (uint8_t)startIdx : 0;
    adaptiveDwellUntil = 0;
    lastCleanupTime = millis();
    lastSaveTime = millis();
    lastMoodTime = millis();
    lastStatsDecay = millis();
    lastCycleActivity = 0;
    huntStartTime = 0;
    lastHuntTime = 0;
    lastHuntChannel = 0;
    dwellResolved = false;
    
    // Reset deferred flags
    pendingPMKIDWrite = 0;
    pendingPMKIDRead = 0;
    pendingPMKIDCount = 0;
    pendingHandshakeCapture = false;
    pendingBeaconStore = false;
    pendingIncompleteWrite = 0;
    pendingIncompleteRead = 0;
    pendingIncompleteCount = 0;
    pendingHandshakeWrite = 0;
    for (uint8_t i = 0; i < PENDING_HS_SLOTS; i++) {
        pendingHandshakeUsed[i] = false;
    }

    // Allocate handshake ring pool (fallback to single slot if allocation fails)
    pendingHandshakePool = &pendingHandshakeFallback;
    pendingHandshakeSlots = 1;
    pendingHandshakePoolAllocated = false;
    void* hsPool = heap_caps_malloc(sizeof(PendingHandshakeFrame) * PENDING_HS_SLOTS, MALLOC_CAP_8BIT);
    if (hsPool) {
        pendingHandshakePool = static_cast<PendingHandshakeFrame*>(hsPool);
        pendingHandshakeSlots = PENDING_HS_SLOTS;
        pendingHandshakePoolAllocated = true;
    }
    
    // CRITICAL: Set running flag with memory barrier
    running = true;
    __sync_synchronize();
    
    // Register our packet callback for EAPOL/PMKID capture
    NetworkRecon::setPacketCallback(promiscuousCallback);
    NetworkRecon::setNewNetworkCallback(onNewNetworkDiscovered);
    
    // UI feedback
    Display::notify(NoticeKind::STATUS, "PEACEFUL VIBES - NO TROUBLE TODAY", 5000, NoticeChannel::TOP_BAR);
    Avatar::setState(AvatarState::NEUTRAL);
    Mood::onPassiveRecon(NetworkRecon::getNetworkCount(), currentChannel);
    Mood::setDialogueLock(true);
    
    Serial.printf("[DNH] Started. Networks available: %d\n", NetworkRecon::getNetworkCount());
}

void DoNoHamMode::stop() {
    if (!running) return;
    
    Serial.println("[DNH] Stopping...");
    SDLog::log("DNH", "Stopping");
    
    running = false;
    
    // Stop grass animation
    Avatar::setGrassMoving(false);
    
    // Clear our packet callback (NetworkRecon keeps running)
    NetworkRecon::setPacketCallback(nullptr);
    NetworkRecon::setNewNetworkCallback(nullptr);
    if (NetworkRecon::isChannelLocked()) {
        NetworkRecon::unlockChannel();
    }
    
    // Process any deferred XP saves
    XP::processPendingSave();
    
    // Process deferred capture saves
    pendingSaveFlag = false;
    bool pausedByUs = false;
    if (NetworkRecon::isRunning()) {
        NetworkRecon::pause();
        pausedByUs = true;
    }
    saveAllPMKIDs();
    saveAllHandshakes();
    if (pausedByUs) {
        NetworkRecon::resume();
    }
    
    // Free per-handshake beacon memory to prevent leaks
    for (auto& hs : handshakes) {
        if (hs.beaconData) {
            free(hs.beaconData);
            hs.beaconData = nullptr;
        }
    }
    
    // Clear DNH-specific vectors only (networks is shared via NetworkRecon)
    dnhBusy = true;
    pmkids.clear();
    pmkids.shrink_to_fit();
    handshakes.clear();
    handshakes.shrink_to_fit();
    dnhBusy = false;
    
    // Reset deferred flags
    pendingPMKIDWrite = 0;
    pendingPMKIDRead = 0;
    pendingPMKIDCount = 0;
    pendingHandshakeCapture = false;
    pendingBeaconStore = false;
    pendingIncompleteWrite = 0;
    pendingIncompleteRead = 0;
    pendingIncompleteCount = 0;
    pendingHandshakeWrite = 0;
    for (uint8_t i = 0; i < PENDING_HS_SLOTS; i++) {
        pendingHandshakeUsed[i] = false;
    }
    if (pendingHandshakePoolAllocated && pendingHandshakePool) {
        free(pendingHandshakePool);
    }
    pendingHandshakePool = &pendingHandshakeFallback;
    pendingHandshakeSlots = 1;
    pendingHandshakePoolAllocated = false;
    Mood::setDialogueLock(false);
}

void DoNoHamMode::update() {
    if (!running) return;
    
    uint32_t now = millis();
    
    // Set busy flag for race protection
    dnhBusy = true;

    // Sync channel state from NetworkRecon
    uint8_t prevChannel = currentChannel;
    currentChannel = NetworkRecon::getCurrentChannel();
    int chanIdx = channelToIndex(currentChannel);
    if (chanIdx >= 0) {
        channelIndex = (uint8_t)chanIdx;
    }
    bool channelChanged = (currentChannel != prevChannel);
    // channelChanged already captures current channel transitions
    
    // Network discovery is handled by NetworkRecon; DNH does not mutate shared networks.
    
    // Process deferred beacon storage for handshakes (ESP32 dual-core race fix)
    // Callback copied beacon to static buffer, we do malloc here in main thread
    uint8_t pendingBeaconBssidLocal[6] = {0};
    uint8_t pendingBeaconDataLocal[512];
    uint16_t pendingBeaconLenLocal = 0;
    bool hasPendingBeacon = false;
    taskENTER_CRITICAL(&pendingBeaconMux);
    if (pendingBeaconStore) {
        memcpy(pendingBeaconBssidLocal, pendingBeaconBSSID, 6);
        pendingBeaconLenLocal = pendingBeaconLen;
        if (pendingBeaconLenLocal > sizeof(pendingBeaconDataLocal)) {
            pendingBeaconLenLocal = sizeof(pendingBeaconDataLocal);
        }
        if (pendingBeaconLenLocal > 0) {
            memcpy(pendingBeaconDataLocal, pendingBeaconData, pendingBeaconLenLocal);
        }
        pendingBeaconStore = false;
        hasPendingBeacon = true;
    }
    taskEXIT_CRITICAL(&pendingBeaconMux);

    if (hasPendingBeacon) {
        for (auto& hs : handshakes) {
            if (!hs.saved && hs.beaconData == nullptr && memcmp(hs.bssid, pendingBeaconBssidLocal, 6) == 0) {
                if (pendingBeaconLenLocal == 0) {
                    break;
                }
                hs.beaconData = (uint8_t*)malloc(pendingBeaconLenLocal);
                if (hs.beaconData) {
                    memcpy(hs.beaconData, pendingBeaconDataLocal, pendingBeaconLenLocal);
                    hs.beaconLen = pendingBeaconLenLocal;
                }
                break;  // One beacon per handshake is enough
            }
        }
    }
    
    // Process deferred PMKID create (ring buffer, head-only)
    PendingPMKIDCreate pendingPMKIDLocal = {};
    bool hasPendingPMKID = false;
    taskENTER_CRITICAL(&pendingPMKIDMux);
    if (pendingPMKIDCount > 0) {
        pendingPMKIDLocal = pendingPMKIDRing[pendingPMKIDRead];
        hasPendingPMKID = true;
    }
    taskEXIT_CRITICAL(&pendingPMKIDMux);

    if (hasPendingPMKID) {
        // Check if dwell is complete (if we needed one)
        bool canProcess = true;
        if (pendingPMKIDLocal.ssid[0] == 0 && state != DNHState::DWELLING) {
            startDwell();
        }
        if (pendingPMKIDLocal.ssid[0] == 0 && state == DNHState::DWELLING) {
            // Still dwelling, wait for beacon or timeout
            if (!dwellResolved && (now - dwellStartTime < DNH_DWELL_TIME)) {
                canProcess = false;
            }
        }

        if (canProcess) {
            // Pop the head entry now that we can process it
            taskENTER_CRITICAL(&pendingPMKIDMux);
            if (pendingPMKIDCount > 0) {
                pendingPMKIDLocal = pendingPMKIDRing[pendingPMKIDRead];
                pendingPMKIDRead = (pendingPMKIDRead + 1) % PENDING_PMKID_SLOTS;
                pendingPMKIDCount--;
            } else {
                canProcess = false;
            }
            taskEXIT_CRITICAL(&pendingPMKIDMux);
        }

        if (canProcess) {
            // Try to find SSID if we don't have it
            if (pendingPMKIDLocal.ssid[0] == 0) {
                int netIdx = NetworkRecon::findNetworkIndex(pendingPMKIDLocal.bssid);
                NetworkRecon::enterCritical();
                if (netIdx >= 0 && netIdx < (int)networks().size() && networks()[netIdx].ssid[0] != 0) {
                    strncpy(pendingPMKIDLocal.ssid, networks()[netIdx].ssid, 32);
                    pendingPMKIDLocal.ssid[32] = 0;
                }
                NetworkRecon::exitCritical();
            }

            // Create or update PMKID entry
            if (pmkids.size() < DNH_MAX_PMKIDS) {
                int idx = findOrCreatePMKID(pendingPMKIDLocal.bssid);
                if (idx >= 0) {
                    memcpy(pmkids[idx].pmkid, pendingPMKIDLocal.pmkid, 16);
                    memcpy(pmkids[idx].station, pendingPMKIDLocal.station, 6);
                    strncpy(pmkids[idx].ssid, pendingPMKIDLocal.ssid, 32);
                    pmkids[idx].ssid[32] = 0;
                    pmkids[idx].timestamp = now;
                    
                    // Announce capture + immediate safe save
                    if (pendingPMKIDLocal.ssid[0] != 0) {
                        Display::showToast("BOOMBOCLAAT! PMKID");
                        // SFX played via Mood::onPMKIDCaptured (don't double-play - causes audio driver issues)
                        Mood::onPMKIDCaptured(pendingPMKIDLocal.ssid);
                        
                        // Immediate save with brief promiscuous pause (safe SD access)
                        bool pausedByUs = false;
                        if (NetworkRecon::isRunning()) {
                            NetworkRecon::pause();
                            pausedByUs = true;
                        }
                        delay(5);
                        saveAllPMKIDs();
                        if (pausedByUs) {
                            NetworkRecon::resume();
                        }
                    }
                }
            }

            // Return to hopping if we were dwelling
            if (state == DNHState::DWELLING) {
                state = DNHState::HOPPING;
                dwellResolved = false;
                adaptiveDwellUntil = 0;
            }
        }
    }

    // Process deferred incomplete handshake tracking (ring buffer)
    while (true) {
        IncompleteHS pendingIncompleteLocal = {};
        bool hasPendingIncomplete = false;
        taskENTER_CRITICAL(&pendingIncompleteMux);
        if (pendingIncompleteCount > 0) {
            pendingIncompleteLocal = pendingIncompleteRing[pendingIncompleteRead];
            pendingIncompleteRead = (pendingIncompleteRead + 1) % PENDING_INCOMPLETE_SLOTS;
            pendingIncompleteCount--;
            hasPendingIncomplete = true;
        }
        taskEXIT_CRITICAL(&pendingIncompleteMux);

        if (!hasPendingIncomplete) {
            break;
        }
        trackIncompleteHandshake(pendingIncompleteLocal.bssid,
                                 pendingIncompleteLocal.capturedMask,
                                 pendingIncompleteLocal.channel);
    }
    
    // Process deferred handshake frame add (ring buffer)
    while (true) {
        PendingHandshakeFrame pendingHandshakeLocal = {};
        bool hasPendingHandshake = false;
        taskENTER_CRITICAL(&pendingHandshakeMux);
        int slot = -1;
        for (uint8_t i = 0; i < pendingHandshakeSlots; i++) {
            if (pendingHandshakeUsed[i]) {
                slot = (int)i;
                break;
            }
        }
        if (slot >= 0) {
            pendingHandshakeLocal = pendingHandshakePool[slot];
            pendingHandshakeUsed[slot] = false;
            pendingHandshakePool[slot].capturedMask = 0;
            hasPendingHandshake = true;
        }
        taskEXIT_CRITICAL(&pendingHandshakeMux);

        if (!hasPendingHandshake) {
            break;
        }

        // Find or create handshake entry
        int hsIdx = findOrCreateHandshake(pendingHandshakeLocal.bssid, pendingHandshakeLocal.station);
        if (hsIdx >= 0) {
            CapturedHandshake& hs = handshakes[hsIdx];
            
            // Process ALL queued frames (M1-M4) from capturedMask
            for (int msgIdx = 0; msgIdx < 4; msgIdx++) {
                if (pendingHandshakeLocal.capturedMask & (1 << msgIdx)) {
                    // Frame is present in the queued data
                    if (hs.frames[msgIdx].len == 0) {  // Not already captured
                        uint16_t copyLen = pendingHandshakeLocal.frames[msgIdx].len;
                        if (copyLen > 0 && copyLen <= 512) {
                            // EAPOL payload for hashcat 22000
                            memcpy(hs.frames[msgIdx].data, pendingHandshakeLocal.frames[msgIdx].data, copyLen);
                            hs.frames[msgIdx].len = copyLen;
                            hs.frames[msgIdx].messageNum = msgIdx + 1;
                            hs.frames[msgIdx].timestamp = now;
                            
                            // Full 802.11 frame for PCAP export (radiotap + WPA-SEC)
                            uint16_t fullCopyLen = pendingHandshakeLocal.frames[msgIdx].fullFrameLen;
                            if (fullCopyLen > 0 && fullCopyLen <= 300) {
                                memcpy(hs.frames[msgIdx].fullFrame, pendingHandshakeLocal.frames[msgIdx].fullFrame, fullCopyLen);
                                hs.frames[msgIdx].fullFrameLen = fullCopyLen;
                                hs.frames[msgIdx].rssi = pendingHandshakeLocal.frames[msgIdx].rssi;
                            }
                            
                            hs.capturedMask |= (1 << msgIdx);
                            hs.lastSeen = now;
                        }
                    }
                }
            }
            
            // Look up SSID if missing
            if (hs.ssid[0] == 0) {
                int netIdx = NetworkRecon::findNetworkIndex(hs.bssid);
                NetworkRecon::enterCritical();
                if (netIdx >= 0 && netIdx < (int)networks().size() && networks()[netIdx].ssid[0] != 0) {
                    strncpy(hs.ssid, networks()[netIdx].ssid, 32);
                    hs.ssid[32] = 0;
                }
                NetworkRecon::exitCritical();
            }
            
            // Check if we just completed a valid pair
            if (hs.hasValidPair() && !hs.saved && !pendingHandshakeCapture) {
                strncpy(pendingHandshakeSSID, hs.ssid, 32);
                pendingHandshakeSSID[32] = 0;
                pendingHandshakeCapture = true;
            }
        }
    }
    
    // Process handshake capture event (UI update + immediate safe save)
    if (pendingHandshakeCapture) {
        Display::showToast("NATURAL HANDSHAKE BLESSED - RESPECT DI HERB");
        // SFX played via Mood::onHandshakeCaptured (don't double-play - causes audio driver issues)
        Mood::onHandshakeCaptured(pendingHandshakeSSID);
        pendingHandshakeCapture = false;
        
        // Immediate save with brief promiscuous pause (safe SD access)
        // ~50ms gap is acceptable - we just captured what we needed
        bool pausedByUs = false;
        if (NetworkRecon::isRunning()) {
            NetworkRecon::pause();
            pausedByUs = true;
        }
        delay(5);  // Let SPI bus settle
        saveAllHandshakes();
        if (pausedByUs) {
            NetworkRecon::resume();
        }
    }
    
    // Periodic beacon data audit to prevent leaks (every 10s - Phase 3A fix)
    static uint32_t lastBeaconAudit = 0;
    if (now - lastBeaconAudit > 10000) {
        for (auto& hs : handshakes) {
            // Free beacon if handshake is saved (no longer needed in RAM)
            if (hs.saved && hs.beaconData) {
                free(hs.beaconData);
                hs.beaconData = nullptr;
                hs.beaconLen = 0;
            }
        }
        lastBeaconAudit = now;
    }
    
    // Channel hopping state machine
    // Sync grass animation with hopping state
    static DNHState lastGrassState = DNHState::HOPPING;
    bool isHopping = (state == DNHState::HOPPING || state == DNHState::IDLE_SWEEP);
    bool wasHopping = (lastGrassState == DNHState::HOPPING || lastGrassState == DNHState::IDLE_SWEEP);
    if (isHopping != wasHopping) {
        Avatar::setGrassMoving(isHopping);
    }
    lastGrassState = state;
    
    switch (state) {
        case DNHState::HOPPING:
            {
                // Clear any expired adaptive dwell lock
                if (adaptiveDwellUntil != 0 && now >= adaptiveDwellUntil) {
                    adaptiveDwellUntil = 0;
                    if (NetworkRecon::isChannelLocked()) {
                        NetworkRecon::unlockChannel();
                    }
                }

                if (adaptiveDwellUntil != 0) {
                    if (checkHuntingTrigger()) {
                        adaptiveDwellUntil = 0;
                    }
                }

                if (adaptiveDwellUntil == 0 && NetworkRecon::isChannelLocked()) {
                    NetworkRecon::unlockChannel();
                }

                if (channelChanged) {
                    // Check if we should enter HUNTING mode after hop
                    bool enteredHunting = checkHuntingTrigger();
                    if (!enteredHunting) {
                        // Check if all channels are dead -> IDLE_SWEEP
                        checkIdleSweep();

                        // Adaptive dwell: extend time on busy channels beyond recon hop interval
                        uint32_t desiredDwell = getAdaptiveHopDelay();
                        uint32_t baseHop = NetworkRecon::getHopIntervalMs();
                        if (desiredDwell > baseHop) {
                            adaptiveDwellUntil = now + (desiredDwell - baseHop);
                            if (!NetworkRecon::isChannelLocked()) {
                                NetworkRecon::lockChannel(currentChannel);
                            }
                        }
                    }
                }
            }
            break;
            
        case DNHState::DWELLING:
            if (!NetworkRecon::isChannelLocked()) {
                NetworkRecon::lockChannel(currentChannel);
            }
            if (dwellResolved || (now - dwellStartTime > DNH_DWELL_TIME)) {
                state = DNHState::HOPPING;
                dwellResolved = false;
                if (NetworkRecon::isChannelLocked()) {
                    NetworkRecon::unlockChannel();
                }
            }
            break;
            
        case DNHState::HUNTING:
            if (!NetworkRecon::isChannelLocked()) {
                NetworkRecon::lockChannel(currentChannel);
            }
            if (now - huntStartTime > HUNT_DURATION) {
                // Hunt timeout, return to hopping
                state = DNHState::HOPPING;
                lastHuntTime = now;
                lastHuntChannel = currentChannel;
                adaptiveDwellUntil = 0;
                if (NetworkRecon::isChannelLocked()) {
                    NetworkRecon::unlockChannel();
                }
            }
            // HUNTING state deliberately does NOT hop - camps on hot channel
            break;
            
        case DNHState::IDLE_SWEEP:
            {
                if (NetworkRecon::isChannelLocked()) {
                    NetworkRecon::unlockChannel();
                }
                adaptiveDwellUntil = 0;

                // If we see ANY activity, exit IDLE_SWEEP
                if (channelChanged) {
                    int idx = (int)channelIndex;
                    if (idx >= 0 && idx < 13) {
                        if (channelStats[idx].beaconCount > 0 || channelStats[idx].eapolCount > 0) {
                            state = DNHState::HOPPING;
                        }
                    }
                }
            }
            break;
    }
    
    // Periodic cleanup (every 10 seconds)
    // NOTE: Network cleanup is now handled centrally by NetworkRecon::cleanupStaleNetworks()
    // to avoid race conditions with shared vector. DNH only prunes its own incomplete handshakes.
    if (now - lastCleanupTime > 10000) {
        pruneIncompleteHandshakes(); // Prune stale handshake tracking (DNH-specific)
        
        // Phase 3A: Shrink vectors after cleanup to reclaim memory
        if (incompleteHandshakes.size() < incompleteHandshakes.capacity() / 2) {
            incompleteHandshakes.shrink_to_fit();
        }
        if (handshakes.size() < handshakes.capacity() / 2) {
            handshakes.shrink_to_fit();
        }
        if (pmkids.size() < pmkids.capacity() / 2) {
            pmkids.shrink_to_fit();
        }
        
        lastCleanupTime = now;
    }
    
    // Periodic stats decay (every 2 minutes)
    if (now - lastStatsDecay > STATS_DECAY_INTERVAL) {
        decayChannelStats();
        lastStatsDecay = now;
    }
    
    // Backup save (every 30 seconds) - catches any missed immediate saves
    // Pause NetworkRecon during SD writes to avoid SPI contention
    if (now - lastSaveTime > 30000) {
        pendingSaveFlag = true;
        lastSaveTime = now;
    }
    if (pendingSaveFlag) {
        pendingSaveFlag = false;
        bool pausedByUs = false;
        if (NetworkRecon::isRunning()) {
            NetworkRecon::pause();
            pausedByUs = true;
        }
        saveAllPMKIDs();
        saveAllHandshakes();
        if (pausedByUs) {
            NetworkRecon::resume();
        }
    }
    
    // Mood update (every 3 seconds)
    if (now - lastMoodTime > 3000) {
        Mood::onPassiveRecon(NetworkRecon::getNetworkCount(), currentChannel);
        lastMoodTime = now;
    }
    
    dnhBusy = false;
}

// Check if channel is primary (1, 6, or 11)
bool DoNoHamMode::isPrimaryChannel(uint8_t ch) {
    return (ch == 1 || ch == 6 || ch == 11);
}

// Calculate adaptive hop delay based on channel activity
uint16_t DoNoHamMode::getAdaptiveHopDelay() {
    ChannelStats& stats = channelStats[channelIndex];
    
    // Base timing: primary channels get more time
    uint16_t baseTime = isPrimaryChannel(stats.channel) ? HOP_BASE_PRIMARY : HOP_BASE_SECONDARY;
    
    // Adjust based on local activity
    uint16_t hopDelay;
    if (stats.beaconCount >= BUSY_THRESHOLD) {
        hopDelay = (baseTime * 3) / 2;  // Busy channel (1.5x)
    } else if (stats.beaconCount >= 2) {
        hopDelay = baseTime;  // Normal activity
    } else if (stats.deadStreak >= DEAD_STREAK_LIMIT) {
        hopDelay = HOP_MIN;  // Dead channel, minimum time
    } else {
        hopDelay = (baseTime * 7) / 10;  // Light activity (0.7x)
    }
    
    // Global activity adjustment
    if (lastCycleActivity < 5) {
        hopDelay = (hopDelay * 3) / 5;  // Quiet spectrum (0.6x)
    } else if (lastCycleActivity > 40) {
        hopDelay = (hopDelay * 6) / 5;  // Busy spectrum (1.2x)
    }
    
    return hopDelay;
}

// Decay channel stats every 2 minutes
void DoNoHamMode::decayChannelStats() {
    for (int i = 0; i < 13; i++) {
        channelStats[i].beaconCount = 0;
        channelStats[i].eapolCount = 0;
        channelStats[i].priority = 100;  // Reset to baseline
        channelStats[i].deadStreak = 0;
    }
    lastCycleActivity = 0;
}

// Check if we should enter HUNTING state
bool DoNoHamMode::checkHuntingTrigger() {
    ChannelStats& stats = channelStats[channelIndex];
    uint32_t now = millis();
    
    // Don't re-hunt same channel too quickly
    if (lastHuntChannel == currentChannel && (now - lastHuntTime) < HUNT_COOLDOWN_MS) {
        return false;
    }
    
    // Trigger: 2+ EAPOL frames or high beacon burst
    if (stats.eapolCount >= 2 || stats.beaconCount >= 8) {
        state = DNHState::HUNTING;
        huntStartTime = now;
        lastHuntChannel = currentChannel;
        lastHuntTime = now;
        adaptiveDwellUntil = 0;
        if (!NetworkRecon::isChannelLocked()) {
            NetworkRecon::lockChannel(currentChannel);
        }
        return true;
    }
    return false;
}

// Check if all channels are dead → IDLE_SWEEP
void DoNoHamMode::checkIdleSweep() {
    // If just completed full cycle
    if (channelIndex == 0) {
        uint16_t totalActivity = 0;
        for (int i = 0; i < 13; i++) {
            totalActivity += channelStats[i].beaconCount;
        }
        lastCycleActivity = totalActivity;
        
        // All channels dead → enter IDLE_SWEEP
        if (totalActivity == 0) {
            state = DNHState::IDLE_SWEEP;
        }
    }
}

// Track incomplete handshake for revisit
void DoNoHamMode::trackIncompleteHandshake(const uint8_t* bssid, uint8_t mask, uint8_t ch) {
    // Check if already tracked
    for (auto& ihs : incompleteHandshakes) {
        if (memcmp(ihs.bssid, bssid, 6) == 0) {
            ihs.capturedMask = mask;
            ihs.lastSeen = millis();
            return;
        }
    }
    
    // Add new if room
    if (incompleteHandshakes.size() < MAX_INCOMPLETE_HS) {
        IncompleteHS ihs;
        memcpy(ihs.bssid, bssid, 6);
        ihs.capturedMask = mask;
        ihs.channel = ch;
        ihs.lastSeen = millis();
        incompleteHandshakes.push_back(ihs);
    }
}

// Remove stale incomplete handshakes
void DoNoHamMode::pruneIncompleteHandshakes() {
    uint32_t now = millis();
    auto it = incompleteHandshakes.begin();
    while (it != incompleteHandshakes.end()) {
        if ((now - it->lastSeen) > INCOMPLETE_HS_TIMEOUT) {
            it = incompleteHandshakes.erase(it);
        } else {
            ++it;
        }
    }
}

void DoNoHamMode::startDwell() {
    state = DNHState::DWELLING;
    dwellStartTime = millis();
    dwellResolved = false;
    adaptiveDwellUntil = 0;
    if (!NetworkRecon::isChannelLocked()) {
        NetworkRecon::lockChannel(currentChannel);
    }
}

// NOTE: ageOutStaleNetworks() removed - network cleanup is now handled centrally
// by NetworkRecon::cleanupStaleNetworks() to avoid race conditions with the shared vector

void DoNoHamMode::saveAllPMKIDs() {
    // Guard: Skip if no SD card available (graceful degradation)
    if (!Config::isSDAvailable()) return;

    const char* handshakesDir = SDLayout::handshakesDir();
    
    // Save PMKIDs in hashcat 22000 format
    for (auto& p : pmkids) {
        if (p.saved) continue;
        if (p.saveAttempts >= 3) continue;  // Give up after 3 failures
        
        // Exponential backoff: 0s, 2s, 5s
        static const uint32_t backoffMs[] = {0, 2000, 5000};
        uint32_t timeSinceCapture = millis() - p.timestamp;
        if (timeSinceCapture < backoffMs[p.saveAttempts]) continue;
        
        // Try to backfill SSID if missing
        if (p.ssid[0] == 0) {
            int netIdx = NetworkRecon::findNetworkIndex(p.bssid);
            NetworkRecon::enterCritical();
            if (netIdx >= 0 && netIdx < (int)networks().size() && networks()[netIdx].ssid[0] != 0) {
                strncpy(p.ssid, networks()[netIdx].ssid, 32);
                p.ssid[32] = 0;
            }
            NetworkRecon::exitCritical();
        }
        
        // Try to backfill SSID from companion txt file (cross-mode compatibility)
        if (p.ssid[0] == 0) {
            char txtPath[64];
            snprintf(txtPath, sizeof(txtPath), "%s/%02X%02X%02X%02X%02X%02X_pmkid.txt",
                     handshakesDir,
                     p.bssid[0], p.bssid[1], p.bssid[2], p.bssid[3], p.bssid[4], p.bssid[5]);
            if (SD.exists(txtPath)) {
                File txtFile = SD.open(txtPath, FILE_READ);
                if (txtFile) {
                    String ssid = txtFile.readStringUntil('\n');
                    ssid.trim();
                    if (ssid.length() > 0) {
                        strncpy(p.ssid, ssid.c_str(), 32);
                        p.ssid[32] = 0;
                    }
                    txtFile.close();
                }
            }
        }
        
        // Can only save if we have SSID (don't count as attempt - will retry when SSID arrives)
        if (p.ssid[0] == 0) continue;
        
        // Check for all-zero PMKID (invalid - don't count as attempt)
        bool allZeros = true;
        for (int i = 0; i < 16; i++) {
            if (p.pmkid[i] != 0) { allZeros = false; break; }
        }
        if (allZeros) {
            p.saved = true;  // Mark invalid PMKID as done
            continue;
        }
        
        // Now we actually attempt to save - increment counter
        p.saveAttempts++;
        
        // Build filename: /handshakes/BSSID.22000
        char filename[64];
        snprintf(filename, sizeof(filename), "%s/%02X%02X%02X%02X%02X%02X.22000",
            handshakesDir,
            p.bssid[0], p.bssid[1], p.bssid[2], p.bssid[3], p.bssid[4], p.bssid[5]);
        
        // Ensure directory exists
        if (!SD.exists(handshakesDir)) {
            SD.mkdir(handshakesDir);
        }
        
        File f = SD.open(filename, FILE_WRITE);
        if (!f) {
            if (p.saveAttempts >= 3) {
                p.saved = true;  // Give up
            }
            continue;
        }
        
        // Build hex strings
        char pmkidHex[33];
        for (int i = 0; i < 16; i++) {
            sprintf(pmkidHex + i*2, "%02x", p.pmkid[i]);
        }
        
        char macAP[13];
        sprintf(macAP, "%02x%02x%02x%02x%02x%02x",
            p.bssid[0], p.bssid[1], p.bssid[2], 
            p.bssid[3], p.bssid[4], p.bssid[5]);
        
        char macClient[13];
        sprintf(macClient, "%02x%02x%02x%02x%02x%02x",
            p.station[0], p.station[1], p.station[2], 
            p.station[3], p.station[4], p.station[5]);
        
        char essidHex[65];
        int ssidLen = strlen(p.ssid);
        if (ssidLen > 32) ssidLen = 32;  // Cap to max SSID length
        for (int i = 0; i < ssidLen; i++) {
            sprintf(essidHex + i*2, "%02x", (uint8_t)p.ssid[i]);
        }
        essidHex[ssidLen * 2] = 0;
        
        // WPA*01*PMKID*MAC_AP*MAC_CLIENT*ESSID***01
        f.printf("WPA*01*%s*%s*%s*%s***01\n", pmkidHex, macAP, macClient, essidHex);
        f.close();
        
        // Save SSID to companion txt file (matches OINK pattern)
        char txtFilename[64];
        snprintf(txtFilename, sizeof(txtFilename), "%s/%02X%02X%02X%02X%02X%02X_pmkid.txt",
                 handshakesDir,
                 p.bssid[0], p.bssid[1], p.bssid[2], p.bssid[3], p.bssid[4], p.bssid[5]);
        // Delete existing file first to ensure clean overwrite (FILE_WRITE appends on ESP32)
        if (SD.exists(txtFilename)) {
            SD.remove(txtFilename);
        }
        File txtFile = SD.open(txtFilename, FILE_WRITE);
        if (txtFile) {
            txtFile.println(p.ssid);
            txtFile.close();
        }
        
        p.saved = true;
        SDLog::log("DNH", "PMKID saved: %s (%s)", p.ssid, filename);
    }
}

void DoNoHamMode::saveAllHandshakes() {
    // Guard: Skip if no SD card available (graceful degradation)
    if (!Config::isSDAvailable()) return;

    const char* handshakesDir = SDLayout::handshakesDir();
    
    // Save handshakes in hashcat 22000 format (WPA*02)
    for (auto& hs : handshakes) {
        if (hs.saved) continue;
        if (!hs.hasValidPair()) continue;  // Need M1+M2 or M2+M3
        if (hs.saveAttempts >= 3) continue;  // Give up after 3 failures
        
        // Exponential backoff: 0s, 2s, 5s
        static const uint32_t backoffMs[] = {0, 2000, 5000};
        uint32_t timeSinceCapture = millis() - hs.lastSeen;
        if (timeSinceCapture < backoffMs[hs.saveAttempts]) continue;
        
        // Try to backfill SSID if missing
        if (hs.ssid[0] == 0) {
            int netIdx = NetworkRecon::findNetworkIndex(hs.bssid);
            NetworkRecon::enterCritical();
            if (netIdx >= 0 && netIdx < (int)networks().size() && networks()[netIdx].ssid[0] != 0) {
                strncpy(hs.ssid, networks()[netIdx].ssid, 32);
                hs.ssid[32] = 0;
            }
            NetworkRecon::exitCritical();
        }
        
        // Try to backfill SSID from companion txt file (cross-mode compatibility)
        if (hs.ssid[0] == 0) {
            char txtPath[64];
            snprintf(txtPath, sizeof(txtPath), "%s/%02X%02X%02X%02X%02X%02X.txt",
                     handshakesDir,
                     hs.bssid[0], hs.bssid[1], hs.bssid[2], hs.bssid[3], hs.bssid[4], hs.bssid[5]);
            if (SD.exists(txtPath)) {
                File txtFile = SD.open(txtPath, FILE_READ);
                if (txtFile) {
                    String ssid = txtFile.readStringUntil('\n');
                    ssid.trim();
                    if (ssid.length() > 0) {
                        strncpy(hs.ssid, ssid.c_str(), 32);
                        hs.ssid[32] = 0;
                    }
                    txtFile.close();
                }
            }
        }
        
        // Can only save if we have SSID (don't count as attempt - will retry when SSID arrives)
        if (hs.ssid[0] == 0) continue;
        
        // Determine message pair
        uint8_t msgPair = hs.getMessagePair();
        if (msgPair == 0xFF) continue;
        
        // Get frames
        const EAPOLFrame* nonceFrame = nullptr;
        const EAPOLFrame* eapolFrame = nullptr;
        
        if (msgPair == 0x00) {
            nonceFrame = &hs.frames[0];  // M1
            eapolFrame = &hs.frames[1];  // M2
        } else {
            nonceFrame = &hs.frames[2];  // M3
            eapolFrame = &hs.frames[1];  // M2
        }
        
        // Frame length validation (don't count as attempt if malformed)
        if (nonceFrame->len < 51 || eapolFrame->len < 97) continue;
        
        // Now we actually attempt to save - increment counter
        hs.saveAttempts++;
        
        // Build filename: /handshakes/BSSID_hs.22000
        char filename[64];
        snprintf(filename, sizeof(filename), "%s/%02X%02X%02X%02X%02X%02X_hs.22000",
            handshakesDir,
            hs.bssid[0], hs.bssid[1], hs.bssid[2], hs.bssid[3], hs.bssid[4], hs.bssid[5]);
        
        // Ensure directory exists
        if (!SD.exists(handshakesDir)) {
            SD.mkdir(handshakesDir);
        }
        
        File f = SD.open(filename, FILE_WRITE);
        if (!f) {
            if (hs.saveAttempts >= 3) {
                hs.saved = true;  // Give up
            }
            continue;
        }
        
        // Extract MIC from M2 (offset 81, 16 bytes)
        char micHex[33];
        for (int i = 0; i < 16; i++) {
            sprintf(micHex + i*2, "%02x", eapolFrame->data[81 + i]);
        }
        
        // MAC_AP
        char macAP[13];
        sprintf(macAP, "%02x%02x%02x%02x%02x%02x",
            hs.bssid[0], hs.bssid[1], hs.bssid[2],
            hs.bssid[3], hs.bssid[4], hs.bssid[5]);
        
        // MAC_CLIENT
        char macClient[13];
        sprintf(macClient, "%02x%02x%02x%02x%02x%02x",
            hs.station[0], hs.station[1], hs.station[2],
            hs.station[3], hs.station[4], hs.station[5]);
        
        // ESSID (hex-encoded, max 32 chars = 64 hex + null)
        char essidHex[65];
        int ssidLen = strlen(hs.ssid);
        if (ssidLen > 32) ssidLen = 32;  // Cap to max SSID length
        for (int i = 0; i < ssidLen; i++) {
            sprintf(essidHex + i*2, "%02x", (uint8_t)hs.ssid[i]);
        }
        essidHex[ssidLen * 2] = 0;
        
        // ANonce from M1 or M3 (offset 17, 32 bytes)
        char nonceHex[65];
        for (int i = 0; i < 32; i++) {
            sprintf(nonceHex + i*2, "%02x", nonceFrame->data[17 + i]);
        }
        
        // EAPOL frame with MIC zeroed
        uint16_t eapolLen = (eapolFrame->data[2] << 8) | eapolFrame->data[3];
        eapolLen += 4;
        if (eapolLen > eapolFrame->len) eapolLen = eapolFrame->len;
        
        char* eapolHex = (char*)malloc(eapolLen * 2 + 1);
        if (!eapolHex) {
            f.close();
            continue;
        }
        
        // Copy and zero MIC
        uint8_t eapolCopy[512];
        memcpy(eapolCopy, eapolFrame->data, eapolLen);
        memset(eapolCopy + 81, 0, 16);
        
        for (int i = 0; i < eapolLen; i++) {
            sprintf(eapolHex + i*2, "%02x", eapolCopy[i]);
        }
        eapolHex[eapolLen * 2] = 0;
        
        // WPA*02*MIC*MAC_AP*MAC_CLIENT*ESSID*ANONCE*EAPOL*MESSAGEPAIR
        f.printf("WPA*02*%s*%s*%s*%s*%s*%s*%02x\n",
            micHex, macAP, macClient, essidHex, nonceHex, eapolHex, msgPair);
        
        free(eapolHex);
        f.close();
        
        // Save SSID to companion txt file (matches OINK pattern)
        char txtFilename[64];
        snprintf(txtFilename, sizeof(txtFilename), "%s/%02X%02X%02X%02X%02X%02X.txt",
                 handshakesDir,
                 hs.bssid[0], hs.bssid[1], hs.bssid[2], hs.bssid[3], hs.bssid[4], hs.bssid[5]);
        if (SD.exists(txtFilename)) {
            SD.remove(txtFilename);
        }
        File txtFile = SD.open(txtFilename, FILE_WRITE);
        if (txtFile) {
            txtFile.println(hs.ssid);
            txtFile.close();
        }
        
        // Also save PCAP (for WPA-SEC upload and wireshark analysis)
        char pcapFilename[64];
        snprintf(pcapFilename, sizeof(pcapFilename), "%s/%02X%02X%02X%02X%02X%02X.pcap",
            handshakesDir,
            hs.bssid[0], hs.bssid[1], hs.bssid[2], hs.bssid[3], hs.bssid[4], hs.bssid[5]);
        
        File pcapFile = SD.open(pcapFilename, FILE_WRITE);
        if (pcapFile) {
            // Write PCAP global header
            DNH_PCAPHeader hdr = {
                .magic = 0xA1B2C3D4,
                .version_major = 2,
                .version_minor = 4,
                .thiszone = 0,
                .sigfigs = 0,
                .snaplen = 65535,
                .linktype = 127  // IEEE802_11_RADIOTAP
            };
            pcapFile.write((uint8_t*)&hdr, sizeof(hdr));
            
            int packetCount = 0;
            
            // Write beacon if available
            if (hs.hasBeacon()) {
                uint32_t beaconTotalLen = sizeof(DNH_RADIOTAP_HEADER) + hs.beaconLen;
                DNH_PCAPPacketHeader beaconPkt = {
                    .ts_sec = hs.firstSeen / 1000,
                    .ts_usec = (hs.firstSeen % 1000) * 1000,
                    .incl_len = beaconTotalLen,
                    .orig_len = beaconTotalLen
                };
                pcapFile.write((uint8_t*)&beaconPkt, sizeof(beaconPkt));
                pcapFile.write(DNH_RADIOTAP_HEADER, sizeof(DNH_RADIOTAP_HEADER));
                pcapFile.write(hs.beaconData, hs.beaconLen);
                packetCount++;
            }
            
            // Write EAPOL frames
            for (int i = 0; i < 4; i++) {
                if (!(hs.capturedMask & (1 << i))) continue;
                const EAPOLFrame& frame = hs.frames[i];
                if (frame.len == 0) continue;
                
                // Prefer fullFrame if available
                if (frame.fullFrameLen > 0 && frame.fullFrameLen <= 300) {
                    uint32_t totalLen = sizeof(DNH_RADIOTAP_HEADER) + frame.fullFrameLen;
                    DNH_PCAPPacketHeader pkt = {
                        .ts_sec = frame.timestamp / 1000,
                        .ts_usec = (frame.timestamp % 1000) * 1000,
                        .incl_len = totalLen,
                        .orig_len = totalLen
                    };
                    pcapFile.write((uint8_t*)&pkt, sizeof(pkt));
                    pcapFile.write(DNH_RADIOTAP_HEADER, sizeof(DNH_RADIOTAP_HEADER));
                    pcapFile.write(frame.fullFrame, frame.fullFrameLen);
                    packetCount++;
                }
            }
            
            pcapFile.close();
        }
        
        hs.saved = true;
        SDLog::log("DNH", "Handshake saved: %s (%s)", hs.ssid, filename);
    }
}

int DoNoHamMode::findNetwork(const uint8_t* bssid) {
    // Delegate to NetworkRecon's thread-safe implementation
    return NetworkRecon::findNetworkIndex(bssid);
}

int DoNoHamMode::findOrCreatePMKID(const uint8_t* bssid) {
    // Find existing
    for (size_t i = 0; i < pmkids.size(); i++) {
        if (memcmp(pmkids[i].bssid, bssid, 6) == 0) {
            return i;
        }
    }
    // Create new
    if (pmkids.size() < DNH_MAX_PMKIDS) {
        if (pmkids.size() >= pmkids.capacity()) {
            size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            if (largest < DNH_PMKID_ALLOC_MIN_BLOCK) {
                return -1;
            }
        }
        CapturedPMKID p = {};
        memcpy(p.bssid, bssid, 6);
        pmkids.push_back(p);
        return pmkids.size() - 1;
    }
    return -1;
}

int DoNoHamMode::findOrCreateHandshake(const uint8_t* bssid, const uint8_t* station) {
    // Find existing with matching BSSID and station
    for (size_t i = 0; i < handshakes.size(); i++) {
        if (memcmp(handshakes[i].bssid, bssid, 6) == 0 &&
            memcmp(handshakes[i].station, station, 6) == 0) {
            return i;
        }
    }
    // Create new
    if (handshakes.size() < DNH_MAX_HANDSHAKES) {
        if (ESP.getFreeHeap() < HeapPolicy::kMinHeapForHandshakeAdd) {
            return -1;
        }
        if (handshakes.size() >= handshakes.capacity()) {
            size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            if (largest < DNH_HANDSHAKE_ALLOC_MIN_BLOCK) {
                return -1;
            }
        }
        CapturedHandshake hs = {};
        memcpy(hs.bssid, bssid, 6);
        memcpy(hs.station, station, 6);
        hs.capturedMask = 0;
        hs.firstSeen = millis();
        hs.lastSeen = hs.firstSeen;
        hs.saved = false;
        hs.beaconData = nullptr;
        hs.beaconLen = 0;
        handshakes.push_back(hs);
        return handshakes.size() - 1;
    }
    return -1;
}

// Inject fake network for stress testing (no RF)
void DoNoHamMode::injectTestNetwork(const uint8_t* bssid, const char* ssid, uint8_t channel, int8_t rssi, wifi_auth_mode_t authmode, bool hasPMF) {
    if (!running) return;
    if (dnhBusy) return;  // Prevent race with update() vector operations
    
    NetworkRecon::enterCritical();
    if (networks().size() >= DNH_MAX_NETWORKS) {
        NetworkRecon::exitCritical();
        return;  // Use consistent constant
    }
    NetworkRecon::exitCritical();
    
    // Heap protection - prevent OOM crash from stress test flooding
    // Need ~300 bytes per DetectedNetwork + vector realloc overhead (can double capacity)
    // Be very conservative: require 80KB free to allow for reallocation headroom
    if (!HeapGates::canGrow(HeapPolicy::kDnhInjectMinHeap,
                            HeapPolicy::kMinFragRatioForGrowth)) {
        // Silently drop - stress test is overwhelming the system
        return;
    }
    
    // Check if already exists (update is cheap, no allocation)
    NetworkRecon::enterCritical();
    for (auto& net : networks()) {
        if (memcmp(net.bssid, bssid, 6) == 0) {
            // Update existing
            net.rssi = rssi;
            net.lastSeen = millis();
            net.beaconCount++;
            NetworkRecon::exitCritical();
            return;
        }
    }
    
    // Add new - wrap in try-catch as last resort OOM protection
    try {
        DetectedNetwork net = {0};
        memcpy(net.bssid, bssid, 6);
        if (ssid && ssid[0]) {
            strncpy(net.ssid, ssid, 32);
            net.ssid[32] = 0;
        }
        net.channel = channel;
        net.rssi = rssi;
        net.authmode = authmode;
        net.hasPMF = hasPMF;
        net.lastSeen = millis();
        net.beaconCount = 1;
        net.isTarget = false;
        net.hasHandshake = false;
        net.attackAttempts = 0;
        net.isHidden = (!ssid || ssid[0] == 0);
        net.clientCount = 0;
        
        networks().push_back(net);
    } catch (...) {
        // OOM during push_back - silently ignore
        Serial.println("[DNH] OOM in injectTestNetwork - dropping");
    }
    NetworkRecon::exitCritical();
}

// Packet callback - registered with NetworkRecon for EAPOL/PMKID capture
void DoNoHamMode::promiscuousCallback(const wifi_promiscuous_pkt_t* pkt, wifi_promiscuous_pkt_type_t type) {
    if (!pkt || !running || dnhBusy) return;
    
    uint16_t len = pkt->rx_ctrl.sig_len;
    // ESP32 adds 4 ghost bytes to sig_len
    if (len > 4) len -= 4;
    if (len < 24) return;
    
    const uint8_t* payload = pkt->payload;
    uint8_t frameSubtype = (payload[0] >> 4) & 0x0F;
    int8_t rssi = pkt->rx_ctrl.rssi;
    
    switch (type) {
        case WIFI_PKT_MGMT:
            if (frameSubtype == 0x08) {  // Beacon
                handleBeacon(payload, len, rssi);
            } else if (frameSubtype == 0x05) {  // Probe Response
                handleProbeResponse(payload, len, rssi);
            }
            break;
        case WIFI_PKT_DATA:
            handleEAPOL(payload, len, rssi);
            break;
        default:
            break;
    }
}

// Frame handlers - called from promiscuousCallback
void DoNoHamMode::handleBeacon(const uint8_t* frame, uint16_t len, int8_t rssi) {
    if (!running) return;
    if (dnhBusy) return;  // Skip if update() is processing vectors
    
    // Beacon frame structure:
    // [0-1] Frame Control, [2-3] Duration, [4-9] DA, [10-15] SA (BSSID), [16-21] BSSID
    // [22-23] Seq, [24-35] Timestamp, [36-37] Beacon Interval, [38-39] Capability
    // [40+] IEs
    
    if (len < 40 || len > 2346) return; // Basic validation: IEEE 802.11 beacon frame limits
    
    const uint8_t* bssid = frame + 16;
    
    // Parse SSID from IE 0
    char ssid[33] = {0};
    uint16_t offset = 36;  // Start of IEs (24-byte header + 12-byte fixed params)
    
    while (offset + 2 < len) {
        if (offset + 2 >= len) break; // Extra bounds check
        uint8_t ieType = frame[offset];
        uint8_t ieLen = frame[offset + 1];
        if (offset + 2 + ieLen > len) break;
        
        if (ieType == 0 && ieLen > 0 && ieLen <= 32) {
            memcpy(ssid, frame + offset + 2, ieLen);
            ssid[ieLen] = 0;
            break;
        }
        offset += 2 + ieLen;
    }
    
    // Check if this resolves a pending PMKID dwell
    if (state == DNHState::DWELLING && ssid[0] != 0) {
        taskENTER_CRITICAL(&pendingPMKIDMux);
        if (pendingPMKIDCount > 0) {
            PendingPMKIDCreate& slot = pendingPMKIDRing[pendingPMKIDRead];
            if (memcmp(bssid, slot.bssid, 6) == 0) {
                strncpy(slot.ssid, ssid, 32);
                slot.ssid[32] = 0;
                dwellResolved = true;
            }
        }
        taskEXIT_CRITICAL(&pendingPMKIDMux);
    }
    
    // Store beacon for any in-progress handshakes from this BSSID
    // (needed for PCAP export / WPA-SEC upload)
    // DEFERRED: Copy beacon to static buffer, let update() do malloc AND matching
    // (ESP32 dual-core race: iterating handshakes here can crash if update() does push_back)
    // BUG FIX: Don't iterate handshakes vector in callback - defer ALL matching to update()
    taskENTER_CRITICAL(&pendingBeaconMux);
    if (!pendingBeaconStore) {
        // Just store the beacon data - update() will find matching handshakes
        uint16_t copyLen = (len > 512) ? 512 : len;
        memcpy(pendingBeaconBSSID, bssid, 6);
        memcpy(pendingBeaconData, frame, copyLen);
        pendingBeaconLen = copyLen;
        pendingBeaconStore = true;
    }
    taskEXIT_CRITICAL(&pendingBeaconMux);
    
    // Track channel activity for adaptive hopping
    int idx = channelToIndex(currentChannel);
    if (idx >= 0 && idx < 13) {
        channelStats[idx].beaconCount++;
        channelStats[idx].lifetimeBeacons++;
        channelStats[idx].lastActivity = millis();
    }
}

void DoNoHamMode::handleProbeResponse(const uint8_t* frame, uint16_t len, int8_t rssi) {
    (void)frame;
    (void)len;
    (void)rssi;
    // NetworkRecon handles probe responses and SSID backfill.
}

void DoNoHamMode::handleEAPOL(const uint8_t* frame, uint16_t len, int8_t rssi) {
    if (!running) return;
    if (dnhBusy) return;  // Skip if update() is processing vectors
    
    // Parse 802.11 data frame to find EAPOL
    // Frame: FC(2) + Duration(2) + Addr1(6) + Addr2(6) + Addr3(6) + Seq(2) = 24 bytes
    // Then QoS(2) if present, then LLC/SNAP(8), then EAPOL payload
    
    if (len < 24 || len > 2346) return; // Basic validation: IEEE 802.11 frame limits
    
    // Check To/From DS flags
    uint8_t toDs = (frame[1] & 0x01);
    uint8_t fromDs = (frame[1] & 0x02) >> 1;
    
    // Extract MACs based on To/From DS
    const uint8_t* srcMac;
    const uint8_t* dstMac;
    const uint8_t* bssid;
    
    if (toDs && !fromDs) {
        // To DS: RA=BSSID, TA=SA
        dstMac = frame + 4;
        srcMac = frame + 10;
        bssid = frame + 4;
    } else if (!toDs && fromDs) {
        // From DS: RA=DA, TA=BSSID
        dstMac = frame + 4;
        srcMac = frame + 10;
        bssid = frame + 10;
    } else if (!toDs && !fromDs) {
        // IBSS or Direct Link
        dstMac = frame + 4;
        srcMac = frame + 10;
        bssid = frame + 16;
    } else {
        // WDS (both set) - skip
        return;
    }
    
    // Calculate data offset (after 802.11 header)
    uint16_t offset = 24;
    
    // Check for QoS Data (subtype bit 3 set)
    uint8_t subtype = (frame[0] >> 4) & 0x0F;
    bool isQoS = (subtype & 0x08) != 0;
    if (isQoS) offset += 2;
    
    // Check for HTC (QoS + Order bit)
    if (isQoS && (frame[1] & 0x80)) offset += 4;
    
    if (offset + 8 > len) return;
    
    // Check LLC/SNAP for EAPOL: AA AA 03 00 00 00 88 8E
    if (frame[offset] != 0xAA || frame[offset+1] != 0xAA ||
        frame[offset+2] != 0x03 || frame[offset+3] != 0x00 ||
        frame[offset+4] != 0x00 || frame[offset+5] != 0x00 ||
        frame[offset+6] != 0x88 || frame[offset+7] != 0x8E) {
        return;  // Not EAPOL
    }
    
    // EAPOL payload starts after LLC/SNAP
    const uint8_t* eapol = frame + offset + 8;
    uint16_t eapolLen = len - offset - 8;
    
    if (eapolLen < 4) return;
    
    // EAPOL: version(1) + type(1) + length(2)
    uint8_t eapolType = eapol[1];
    if (eapolType != 3) return;  // EAPOL-Key only
    
    if (eapolLen < 99) return;  // Minimum for key frame
    
    // EAPOL-Key: descriptor_type(1) @ 4, key_info(2) @ 5-6
    uint16_t keyInfo = (eapol[5] << 8) | eapol[6];
    uint8_t install = (keyInfo >> 6) & 0x01;
    uint8_t keyAck = (keyInfo >> 7) & 0x01;
    uint8_t keyMic = (keyInfo >> 8) & 0x01;
    uint8_t secure = (keyInfo >> 9) & 0x01;
    
    // Identify message: M1 = KeyAck, no MIC; M2 = MIC, not secure; M3 = KeyAck+MIC+Install; M4 = MIC+Secure
    uint8_t messageNum = 0;
    if (keyAck && !keyMic) messageNum = 1;
    else if (!keyAck && keyMic && !secure) messageNum = 2;
    else if (keyAck && keyMic && install) messageNum = 3;
    else if (!keyAck && keyMic && secure) messageNum = 4;
    
    if (messageNum == 0) return;  // Unknown message type
    
    // Determine BSSID and station based on message direction
    // M1/M3 = AP->Station, M2/M4 = Station->AP
    uint8_t apBssid[6], station[6];
    if (messageNum == 1 || messageNum == 3) {
        memcpy(apBssid, srcMac, 6);
        memcpy(station, dstMac, 6);
    } else {
        memcpy(apBssid, dstMac, 6);
        memcpy(station, srcMac, 6);
    }
    
    // ========== PMKID EXTRACTION FROM M1 ==========
    if (messageNum == 1) {
        // Descriptor type 0x02 = RSN (WPA2/WPA3), 0xFE = WPA1
        uint8_t descriptorType = eapol[4];
        if (descriptorType == 0x02 && eapolLen >= 121) {  // WPA2/3 only
            uint16_t keyDataLen = (eapol[97] << 8) | eapol[98];
            if (keyDataLen >= 22 && eapolLen >= 99 + keyDataLen) {
                const uint8_t* keyData = eapol + 99;
                
                // Search for PMKID KDE: dd 14 00 0f ac 04 [16-byte PMKID]
                for (uint16_t i = 0; i + 22 <= keyDataLen; i++) {
                    if (keyData[i] == 0xdd && keyData[i+1] == 0x14 &&
                        keyData[i+2] == 0x00 && keyData[i+3] == 0x0f &&
                        keyData[i+4] == 0xac && keyData[i+5] == 0x04) {
                        
                        const uint8_t* pmkidData = keyData + i + 6;
                        
                        // Skip all-zero PMKIDs (invalid)
                        bool allZeros = true;
                        for (int z = 0; z < 16; z++) {
                            if (pmkidData[z] != 0) { allZeros = false; break; }
                        }
                        if (allZeros) {
                            break;
                        }
                        
                        // Queue PMKID for creation in main thread (ring buffer)
                        taskENTER_CRITICAL(&pendingPMKIDMux);
                        if (pendingPMKIDCount < PENDING_PMKID_SLOTS) {
                            PendingPMKIDCreate& slot = pendingPMKIDRing[pendingPMKIDWrite];
                            memcpy(slot.bssid, apBssid, 6);
                            memcpy(slot.station, station, 6);
                            memcpy(slot.pmkid, pmkidData, 16);
                            slot.channel = currentChannel;
                            
                            // BUG FIX: Don't call findNetwork() from callback - race condition!
                            // SSID lookup deferred to update() where it's safe
                            // Clear SSID to trigger dwell/lookup in update()
                            slot.ssid[0] = 0;
                            
                            pendingPMKIDWrite = (pendingPMKIDWrite + 1) % PENDING_PMKID_SLOTS;
                            pendingPMKIDCount++;
                        }
                        taskEXIT_CRITICAL(&pendingPMKIDMux);
                        break;  // Found PMKID, stop searching
                    }
                }
            }
        }
    }
    
    // ========== HANDSHAKE FRAME CAPTURE (M1-M4) ==========
    // Queue frame for deferred processing (natural client reconnects)
    // Batch accumulate if frames arrive in quick succession
    taskENTER_CRITICAL(&pendingHandshakeMux);
    int slot = -1;
    // Prefer existing slot for same handshake
    for (uint8_t i = 0; i < pendingHandshakeSlots; i++) {
        if (pendingHandshakeUsed[i] &&
            memcmp(pendingHandshakePool[i].bssid, apBssid, 6) == 0 &&
            memcmp(pendingHandshakePool[i].station, station, 6) == 0) {
            slot = (int)i;
            break;
        }
    }
    // Otherwise, pick a free slot
    if (slot < 0) {
        for (uint8_t i = 0; i < pendingHandshakeSlots; i++) {
            uint8_t idx = (pendingHandshakeWrite + i) % pendingHandshakeSlots;
            if (!pendingHandshakeUsed[idx]) {
                slot = (int)idx;
                pendingHandshakeWrite = (idx + 1) % pendingHandshakeSlots;
                // Init the slot for this handshake
                memcpy(pendingHandshakePool[idx].bssid, apBssid, 6);
                memcpy(pendingHandshakePool[idx].station, station, 6);
                pendingHandshakePool[idx].messageNum = messageNum;  // DEPRECATED
                pendingHandshakePool[idx].capturedMask = 0;
                for (int f = 0; f < 4; f++) {
                    pendingHandshakePool[idx].frames[f].len = 0;
                    pendingHandshakePool[idx].frames[f].fullFrameLen = 0;
                }
                break;
            }
        }
    }

    if (slot >= 0) {
        // Store this frame in the appropriate slot (M1-M4 → index 0-3)
        uint8_t frameIdx = messageNum - 1;
        if (frameIdx < 4) {
            PendingHandshakeFrame& slotRef = pendingHandshakePool[slot];
            // EAPOL payload for hashcat 22000
            uint16_t copyLen = min((uint16_t)512, eapolLen);
            memcpy(slotRef.frames[frameIdx].data, eapol, copyLen);
            slotRef.frames[frameIdx].len = copyLen;
            
            // Full 802.11 frame for PCAP export (radiotap + WPA-SEC compatibility)
            uint16_t fullCopyLen = min((uint16_t)300, len);
            memcpy(slotRef.frames[frameIdx].fullFrame, frame, fullCopyLen);
            slotRef.frames[frameIdx].fullFrameLen = fullCopyLen;
            slotRef.frames[frameIdx].rssi = rssi;
            
            slotRef.capturedMask |= (1 << frameIdx);
            pendingHandshakeUsed[slot] = true;
        }
    }
    taskEXIT_CRITICAL(&pendingHandshakeMux);
    
    // Track channel activity for adaptive hopping
    int idx = channelToIndex(currentChannel);
    if (idx >= 0 && idx < 13) {
        channelStats[idx].eapolCount++;
        channelStats[idx].lastActivity = millis();
    }
    
    // Track incomplete handshakes for future hunting (defer to update)
    uint8_t captureMask = (1 << (messageNum - 1));
    taskENTER_CRITICAL(&pendingIncompleteMux);
    if (pendingIncompleteCount < PENDING_INCOMPLETE_SLOTS) {
        IncompleteHS& slot = pendingIncompleteRing[pendingIncompleteWrite];
        memcpy(slot.bssid, apBssid, 6);
        slot.capturedMask = captureMask;
        slot.channel = currentChannel;
        slot.lastSeen = millis();
        pendingIncompleteWrite = (pendingIncompleteWrite + 1) % PENDING_INCOMPLETE_SLOTS;
        pendingIncompleteCount++;
    }
    taskEXIT_CRITICAL(&pendingIncompleteMux);
}
