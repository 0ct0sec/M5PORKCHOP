// NetworkRecon - Background WiFi Reconnaissance Service Implementation
// Provides shared network scanning for OINK, DONOHAM, and SPECTRUM modes

#include "network_recon.h"
#include "../modes/oink.h"  // For DetectedNetwork, DetectedClient types
#include "config.h"
#include "wsl_bypasser.h"
#include "wifi_utils.h"
#include "heap_gates.h"
#include "heap_policy.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <NimBLEDevice.h>
#include <atomic>
#include <SD.h>
#include "sd_layout.h"
#include "flock_detect.h"
#include "flock_log.h"
#include "../defense/attack_detect.h"
#include "../defense/eviltwin_detect.h"
#include "xp.h"
#include "../piglet/mood.h"
#include "../gps/gps.h"
#include "../audio/sfx.h"
#include "../ui/display.h"

// Serial-print the flock pipeline counters (match/enqueue/drain/alert) from
// serviceFlockAlerts() for bring-up. Off by default now DNH is confirmed;
// flip to 1 to diagnose a silent detector. The atomic counters themselves stay
// live (they're cheap) so GUARD HOG and future diagnostics can read them.
#ifndef FLOCK_DEBUG_COUNTERS
#define FLOCK_DEBUG_COUNTERS 0
#endif

namespace NetworkRecon {

// ============================================================================
// State Variables
// ============================================================================

static bool initialized = false;
static bool running = false;
static bool paused = false;
static std::atomic<bool> channelLocked{false};
static bool channelLockedBeforePause = false;  // [BUG4 FIX] Save state for pause/resume
static uint8_t lockedChannel = 0;
static uint8_t currentChannel = 1;
static uint8_t currentChannelIndex = 0;
static uint32_t lastHopTime = 0;
static uint32_t lastCleanupTime = 0;
static uint32_t startTime = 0;
static std::atomic<uint32_t> packetCount{0};
static std::atomic<bool> busy{false};  // [BUG3 FIX] Atomic for cross-core visibility
static std::atomic<uint32_t> hopIntervalOverrideMs{0};
static size_t heapLargestAtStart = 0;
static bool heapStabilized = false;
// shrinkDeferCount removed — shrink_to_fit no longer runs during operation

// Channel hop order (most common channels first for faster discovery)
static const uint8_t CHANNEL_HOP_ORDER[RECON_CHANNEL_COUNT] = {
    1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13
};

// Stale network timeout (remove if not seen for this long)
static const uint32_t STALE_TIMEOUT_MS = 60000;

// Cleanup interval
static const uint32_t CLEANUP_INTERVAL_MS = 5000;

// Client activity decay (clear bitset after inactivity)
static const uint32_t CLIENT_BITMAP_RESET_MS = 30000;

static uint32_t getHopIntervalMsInternal() {
    uint32_t overrideMs = hopIntervalOverrideMs.load();
    uint32_t interval = overrideMs > 0 ? overrideMs : Config::wifi().channelHopInterval;
    if (interval < 50) interval = 50;
    if (interval > 2000) interval = 2000;
    return interval;
}

// Beacon interval sanity cap (ignore huge gaps for EMA)
static const uint32_t BEACON_INTERVAL_MAX_MS = 5000;

// ============================================================================
// Thread Safety
// ============================================================================

static portMUX_TYPE vectorMux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================================
// Shared Data
// ============================================================================

static std::vector<DetectedNetwork> networks;

// ============================================================================
// Deferred Event Processing (avoid allocations in callback)
// ============================================================================

static const uint8_t PENDING_NET_SLOTS = 16;
static DetectedNetwork pendingNetworks[PENDING_NET_SLOTS];
static std::atomic<uint8_t> pendingNetWrite{0};
static std::atomic<uint8_t> pendingNetRead{0};

// Pending SSID reveal cache (best-effort for hidden networks)
static const uint8_t PENDING_SSID_SLOTS = 4;
struct PendingSSID {
    uint8_t bssid[6];
    char ssid[33];
    std::atomic<bool> ready{false};
};
static PendingSSID pendingSsids[PENDING_SSID_SLOTS];
static std::atomic<uint8_t> pendingSsidWrite{0};

// ============================================================================
// Mode-Specific Callbacks
// ============================================================================

static std::atomic<PacketCallback> modeCallback{nullptr};
static NewNetworkCallback newNetworkCallback = nullptr;

// ============================================================================
// FLOCK DETECTION (passive counter-surveillance)
// Runs on every promiscuous frame while any passive mode (DO NO HAM / OINK /
// SPECTRUM) is active. Detection is cheap and done in the packet callback;
// the siren + GPS-tagged SD log are deferred to update() (main-loop context)
// via a lock-free ring buffer, mirroring the pendingNetworks pattern.
// ============================================================================
static flockdet::FlockDetect g_flockDet;

// SQUEAL ALERT — passive 802.11 attack monitor, fed from promiscuousCallback,
// rolled + alerted from serviceFlockAlerts() on the main loop.
static attackdet::AttackMonitor g_attackMon;
static attackdet::AttackType    g_lastAttack = attackdet::AttackType::None;
static uint32_t                 g_lastAttackAlertMs = 0;

// FAKE BACON — evil-twin monitor. Fed a compact AP observation per newly-seen
// beacon via a lock-free ring (callback-safe); the table logic + alert run on
// the main loop in serviceFlockAlerts().
static eviltwin::EvilTwinMonitor g_evilTwin;
struct ApObs { char ssid[33]; uint8_t bssid[6]; uint8_t open; };
static const uint8_t AP_OBS_SLOTS = 8;
static ApObs apObsRing[AP_OBS_SLOTS];
static std::atomic<uint8_t> apObsWrite{0};
static std::atomic<uint8_t> apObsRead{0};
static uint8_t g_evilTwinCount = 0;

// callback-safe: copy an AP observation into the ring (no alloc/IO)
static inline void enqueueApObs(const char* ssid, const uint8_t* bssid, bool open) {
    if (!ssid || ssid[0] == '\0') return;   // skip hidden
    uint8_t w = apObsWrite.load(std::memory_order_relaxed);
    uint8_t nxt = (uint8_t)((w + 1) % AP_OBS_SLOTS);
    if (nxt == apObsRead.load(std::memory_order_acquire)) return;  // full: drop
    strncpy(apObsRing[w].ssid, ssid, 32);
    apObsRing[w].ssid[32] = '\0';
    memcpy(apObsRing[w].bssid, bssid, 6);
    apObsRing[w].open = open ? 1 : 0;
    apObsWrite.store(nxt, std::memory_order_release);
}

struct FlockHitRec {
    uint8_t     mac[6];
    int8_t      rssi;
    uint8_t     channel;
    uint8_t     kind;        // flockdet::DeviceKind
    uint8_t     confidence;  // flockdet::Confidence
    const char* reason;
};
static const uint8_t FLOCK_HIT_SLOTS = 8;
static FlockHitRec flockHits[FLOCK_HIT_SLOTS];
static std::atomic<uint8_t> flockHitWrite{0};
static std::atomic<uint8_t> flockHitRead{0};

static const uint8_t FLOCK_SEEN_MAX = 64;
static uint8_t flockSeen[FLOCK_SEEN_MAX][6];
static uint8_t flockSeenCount = 0;
static char flockCsvFilename[128] = {0};

// [TEMP DEBUG — strip once DNH confirmed] counters to prove enqueue vs drain.
// enqueue climbs but drain stays 0  => drain path never runs in this mode.
// enqueue stays 0                   => frames never match (detection/frame path).
static std::atomic<uint32_t> flockMatchCount{0};    // inspectWifiFrame().hit()
static std::atomic<uint32_t> flockEnqueueCount{0};  // successfully rang the buffer
static std::atomic<uint32_t> flockDrainCount{0};    // dequeued in main loop
static std::atomic<uint32_t> flockAlertCount{0};    // passed dedup -> siren+log

// callback-safe: copy hit into ring buffer, no allocation/IO
static inline void enqueueFlockHit(const flockdet::Detection& d) {
    uint8_t w = flockHitWrite.load(std::memory_order_relaxed);
    uint8_t nxt = (uint8_t)((w + 1) % FLOCK_HIT_SLOTS);
    if (nxt == flockHitRead.load(std::memory_order_acquire)) return; // full: drop
    flockEnqueueCount.fetch_add(1, std::memory_order_relaxed);
    FlockHitRec& r = flockHits[w];
    memcpy(r.mac, d.mac, 6);
    r.rssi = d.rssi;
    r.channel = d.channel;
    r.kind = (uint8_t)d.kind;
    r.confidence = (uint8_t)d.confidence;
    r.reason = d.reason;
    flockHitWrite.store(nxt, std::memory_order_release);
}

static bool flockAlreadySeen(const uint8_t* mac) {
    for (uint8_t i = 0; i < flockSeenCount; i++)
        if (memcmp(flockSeen[i], mac, 6) == 0) return true;
    return false;
}
static void flockMarkSeen(const uint8_t* mac) {
    if (flockSeenCount < FLOCK_SEEN_MAX) memcpy(flockSeen[flockSeenCount++], mac, 6);
}

static bool ensureFlockFile() {
    if (flockCsvFilename[0] != '\0') return true;
    const char* dir = SDLayout::wardrivingDir();
    if (!SD.exists(dir)) { if (!SD.mkdir(dir)) return false; }
    GPSData g = GPS::getData();
    if (g.date > 0 && g.time > 0) {
        uint8_t day=g.date/10000, mon=(g.date/100)%100, yr=g.date%100;
        uint8_t hh=g.time/1000000, mm=(g.time/10000)%100, ss=(g.time/100)%100;
        snprintf(flockCsvFilename, sizeof(flockCsvFilename),
                 "%s/flock_20%02d%02d%02d_%02d%02d%02d.csv", dir, yr,mon,day,hh,mm,ss);
    } else {
        snprintf(flockCsvFilename, sizeof(flockCsvFilename),
                 "%s/flock_%lu.csv", dir, (unsigned long)millis());
    }
    File f = SD.open(flockCsvFilename, FILE_WRITE);
    if (!f) { flockCsvFilename[0] = '\0'; return false; }
    f.print(flockdet::flockCsvHeader());
    f.close();
    return true;
}

// main-loop context: de-dup, siren, GPS-tag, SD log for new hits
static void drainFlockHits() {
    while (flockHitRead.load(std::memory_order_relaxed) !=
           flockHitWrite.load(std::memory_order_acquire)) {
        uint8_t r = flockHitRead.load(std::memory_order_relaxed);
        FlockHitRec rec = flockHits[r];
        flockHitRead.store((uint8_t)((r + 1) % FLOCK_HIT_SLOTS), std::memory_order_release);
        flockDrainCount.fetch_add(1, std::memory_order_relaxed);

        if (flockAlreadySeen(rec.mac)) continue;   // one alert per device per session
        flockMarkSeen(rec.mac);
        flockAlertCount.fetch_add(1, std::memory_order_relaxed);

        bool raven = (rec.kind == (uint8_t)flockdet::DeviceKind::RavenDetector);
        Display::showToast(raven ? "RAVEN NEARBY" : "FLOCK CAM NEAR");
        SFX::play(raven ? SFX::PIG_RAVEN : SFX::PIG_ALARM);
        // DEFENSIVE catch: a new camera/Raven (deduped per MAC above) — XP + fatten.
        XP::addXP(XP_DEFENSE_HIT);
        Mood::onDefensiveCatch();

        if (Config::isSDAvailable() && ensureFlockFile()) {
            flockdet::Detection d;
            d.kind = (flockdet::DeviceKind)rec.kind;
            d.confidence = (flockdet::Confidence)rec.confidence;
            memcpy(d.mac, rec.mac, 6);
            d.rssi = rec.rssi; d.channel = rec.channel; d.reason = rec.reason;

            flockdet::GpsFix fix;
            GPSData g = GPS::getData();
            if (g.latitude != 0.0 || g.longitude != 0.0) {
                fix.lat = g.latitude; fix.lon = g.longitude;
                fix.altM = (float)g.altitude;
                fix.accM = (float)(g.hdop > 0 ? g.hdop * 5.0 : 10.0);
                if (g.date > 0 && g.time > 0) {
                    uint8_t day=g.date/10000, mon=(g.date/100)%100, yr=g.date%100;
                    uint8_t hh=g.time/1000000, mm=(g.time/10000)%100, ss=(g.time/100)%100;
                    snprintf(fix.utc, sizeof(fix.utc), "20%02d-%02d-%02dT%02d:%02d:%02dZ",
                             yr,mon,day,hh,mm,ss);
                    fix.valid = true;
                }
            }
            char line[192];
            int n = flockdet::flockCsvRow(line, sizeof(line), d, fix);
            if (n > 0) {
                File f = SD.open(flockCsvFilename, FILE_APPEND);
                if (f) { f.print(line); f.close(); }
            }
        }
    }
}

// ============================================================================
// Internal Functions
// ============================================================================

static void hopChannel() {
    if (channelLocked.load(std::memory_order_acquire)) return;
    
    currentChannelIndex = (currentChannelIndex + 1) % RECON_CHANNEL_COUNT;
    currentChannel = CHANNEL_HOP_ORDER[currentChannelIndex];
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
}

static int findNetworkInternal(const uint8_t* bssid) {
    for (size_t i = 0; i < networks.size(); i++) {
        if (memcmp(networks[i].bssid, bssid, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static inline int8_t updateRssiAvg(int8_t prev, int8_t sample) {
    if (prev == 0) return sample;
    int16_t blended = (int16_t)prev * 3 + (int16_t)sample;
    return (int8_t)(blended / 4);
}

static bool enqueuePendingNetwork(const DetectedNetwork& net) {
    uint8_t write = pendingNetWrite.load(std::memory_order_relaxed);
    uint8_t next = (uint8_t)((write + 1) % PENDING_NET_SLOTS);
    uint8_t read = pendingNetRead.load(std::memory_order_acquire);
    if (next == read) {
        return false;  // Queue full, drop
    }
    pendingNetworks[write] = net;
    pendingNetWrite.store(next, std::memory_order_release);
    return true;
}

static bool dequeuePendingNetwork(DetectedNetwork& out) {
    uint8_t read = pendingNetRead.load(std::memory_order_relaxed);
    uint8_t write = pendingNetWrite.load(std::memory_order_acquire);
    if (read == write) {
        return false;
    }
    out = pendingNetworks[read];
    pendingNetRead.store((uint8_t)((read + 1) % PENDING_NET_SLOTS), std::memory_order_release);
    return true;
}

static void storePendingSsid(const uint8_t* bssid, const char* ssid) {
    if (!bssid || !ssid || ssid[0] == 0) return;
    uint8_t slot = pendingSsidWrite.fetch_add(1, std::memory_order_relaxed) % PENDING_SSID_SLOTS;
    pendingSsids[slot].ready.store(false, std::memory_order_relaxed);
    memcpy(pendingSsids[slot].bssid, bssid, 6);
    strncpy(pendingSsids[slot].ssid, ssid, 32);
    pendingSsids[slot].ssid[32] = 0;
    pendingSsids[slot].ready.store(true, std::memory_order_release);
}

static bool applyPendingSsid(DetectedNetwork& net) {
    for (uint8_t i = 0; i < PENDING_SSID_SLOTS; i++) {
        if (!pendingSsids[i].ready.load(std::memory_order_acquire)) continue;
        if (memcmp(pendingSsids[i].bssid, net.bssid, 6) != 0) continue;
        strncpy(net.ssid, pendingSsids[i].ssid, 32);
        net.ssid[32] = 0;
        net.isHidden = false;
        pendingSsids[i].ready.store(false, std::memory_order_release);
        return true;
    }
    return false;
}

static void revealSsidIfKnown(const uint8_t* bssid, const char* ssid) {
    if (!bssid || !ssid || ssid[0] == 0) return;

    // Try to apply to existing network first
    taskENTER_CRITICAL(&vectorMux);
    int idx = findNetworkInternal(bssid);
    if (idx >= 0 && idx < (int)networks.size()) {
        if (networks[idx].ssid[0] == 0 || networks[idx].isHidden) {
            strncpy(networks[idx].ssid, ssid, 32);
            networks[idx].ssid[32] = 0;
            networks[idx].isHidden = false;
            networks[idx].lastSeen = millis();
        }
        taskEXIT_CRITICAL(&vectorMux);
        return;
    }
    taskEXIT_CRITICAL(&vectorMux);

    // Otherwise, store for when the network is added
    storePendingSsid(bssid, ssid);
}

static inline uint8_t clientHashIndex(const uint8_t* mac) {
    uint32_t h = 2166136261u;  // FNV-1a
    for (int i = 0; i < 6; i++) {
        h ^= mac[i];
        h *= 16777619u;
    }
    return (uint8_t)(h & 0x7F);
}

static int computeRetentionScore(const DetectedNetwork& net, uint32_t now) {
    int8_t rssi = (net.rssiAvg != 0) ? net.rssiAvg : net.rssi;
    int score = 0;

    if (rssi <= -95) score += 0;
    else if (rssi >= -30) score += 60;
    else score += ((int)rssi + 95) * 60 / 65;

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

    if (net.hasHandshake) score -= 20;
    if (net.hasPMF) score -= 15;
    if (net.authmode == WIFI_AUTH_OPEN) score -= 10;
    if (net.ssid[0] == 0 || net.isHidden) score -= 10;
    if (net.cooldownUntil > now) score -= 10;

    return score;
}

// PMF detection result - we need both bits to distinguish WPA3 from WPA2/WPA3 mixed
// jader242 i know you reading dis. pig knows.
struct PmfResult {
    bool capable;   // MFPC (bit 6) - PMF capable (WPA2/WPA3 transitional sets this)
    bool required;  // MFPR (bit 7) - PMF required (pure WPA3 sets both)
};

static PmfResult detectPMF(const uint8_t* payload, uint16_t len) {
    PmfResult result = {false, false};
    uint16_t offset = 36;
    while (offset + 2 < len) {
        uint8_t id = payload[offset];
        uint8_t ieLen = payload[offset + 1];

        if (offset + 2 + ieLen > len) break;

        if (id == 0x30 && ieLen >= 8) {  // RSN IE
            uint16_t rsnOffset = offset + 2;
            uint16_t rsnEnd = rsnOffset + ieLen;
            uint16_t version = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            if (version != 1) {
                offset += 2 + ieLen;
                continue;
            }

            // Walk the RSN IE properly - no shortcuts, no assumptions
            rsnOffset += 6;  // Skip version(2) + group cipher(4)
            if (rsnOffset + 2 > rsnEnd) break;

            uint16_t pairwiseCount = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            rsnOffset += 2 + (pairwiseCount * 4);
            if (rsnOffset + 2 > rsnEnd) break;

            uint16_t akmCount = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            rsnOffset += 2 + (akmCount * 4);
            if (rsnOffset + 2 > rsnEnd) break;

            // RSN Capabilities - IEEE 802.11-2016 Table 9-133
            // Bit 6: MFPC (Management Frame Protection Capable)
            // Bit 7: MFPR (Management Frame Protection Required)
            uint16_t caps = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            result.capable  = (caps >> 6) & 0x01;  // MFPC
            result.required = (caps >> 7) & 0x01;  // MFPR
            break;
        }

        offset += 2 + ieLen;
    }
    return result;
}

static void processBeacon(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (len < 36) return;
    
    const uint8_t* bssid = payload + 16;
    PmfResult pmf = detectPMF(payload, len);
    uint32_t now = millis();
    
    // [BUG1 FIX] Lookup under spinlock - vector can be modified by cleanupStaleNetworks()
    taskENTER_CRITICAL(&vectorMux);
    int idx = findNetworkInternal(bssid);
    taskEXIT_CRITICAL(&vectorMux);
    
    if (idx < 0) {
        // New network - queue for deferred add
        DetectedNetwork net = {0};
        memcpy(net.bssid, bssid, 6);
        net.rssi = rssi;
        net.rssiAvg = rssi;
        net.channel = currentChannel;
        net.authmode = WIFI_AUTH_OPEN;
        net.firstSeen = now;
        net.lastSeen = now;
        net.lastBeaconSeen = now;
        net.beaconCount = 1;
        net.beaconIntervalEmaMs = 0;
        net.isTarget = false;
        net.hasPMF = pmf.required;
        net.hasHandshake = false;
        net.attackAttempts = 0;
        net.isHidden = false;
        net.lastDataSeen = 0;
        net.cooldownUntil = 0;
        net.clientBitset = 0;
        net.clientBitsetHigh = 0;

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
                    
                    // Check for all-null SSID (hidden)
                    bool allNull = true;
                    for (uint8_t i = 0; i < ieLen; i++) {
                        if (net.ssid[i] != 0) { allNull = false; break; }
                    }
                    if (allNull) {
                        net.isHidden = true;
                    }
                } else if (ieLen == 0) {
                    net.isHidden = true;
                }
                break;
            }
            
            offset += 2 + ieLen;
        }
        
        // Get channel from DS Parameter Set IE
        offset = 36;
        while (offset + 2 < len) {
            uint8_t id = payload[offset];
            uint8_t ieLen = payload[offset + 1];
            
            if (id == 3 && ieLen == 1) {
                net.channel = payload[offset + 2];
                break;
            }
            
            offset += 2 + ieLen;
        }
        
        // Parse auth mode from RSN (0x30) and WPA vendor (0xDD) IEs
        // RSN + MFPR        = pure WPA3 (PMF required, no fallback)
        // RSN + MFPC only   = WPA2/WPA3 transitional (PMF capable, clients choose)
        // RSN alone          = WPA2
        // WPA vendor alone   = WPA1
        // RSN + WPA vendor   = WPA1/WPA2 mixed
        bool hasRSN = false;
        offset = 36;
        while (offset + 2 < len) {
            uint8_t id = payload[offset];
            uint8_t ieLen = payload[offset + 1];

            if (offset + 2 + ieLen > len) break;

            if (id == 0x30 && ieLen >= 2) {  // RSN IE
                hasRSN = true;
                net.authmode = WIFI_AUTH_WPA2_PSK;
            } else if (id == 0xDD && ieLen >= 8) {  // Vendor specific
                if (payload[offset + 2] == 0x00 && payload[offset + 3] == 0x50 &&
                    payload[offset + 4] == 0xF2 && payload[offset + 5] == 0x01) {
                    if (!hasRSN) {
                        net.authmode = WIFI_AUTH_WPA_PSK;
                    } else {
                        net.authmode = WIFI_AUTH_WPA_WPA2_PSK;
                    }
                }
            }

            offset += 2 + ieLen;
        }

        // Apply PMF classification on top of RSN detection
        if (hasRSN && net.authmode == WIFI_AUTH_WPA2_PSK) {
            if (pmf.required) {
                net.authmode = WIFI_AUTH_WPA3_PSK;         // MFPR=1: pure WPA3-SAE
            } else if (pmf.capable) {
                net.authmode = WIFI_AUTH_WPA2_WPA3_PSK;    // MFPC=1 only: transitional
            }
        }
        
        if (net.channel == 0) {
            net.channel = currentChannel;
        }

        // FAKE BACON: feed the evil-twin monitor one observation per new AP.
        // We have a fully-parsed SSID + BSSID + auth here.
        if (!net.isHidden) {
            enqueueApObs(net.ssid, net.bssid, net.authmode == WIFI_AUTH_OPEN);
        }

        // Queue for deferred add
        enqueuePendingNetwork(net);
    } else {
        // Update existing network
        taskENTER_CRITICAL(&vectorMux);
        if (idx >= 0 && idx < (int)networks.size()) {
            DetectedNetwork& net = networks[idx];
            net.rssi = rssi;
            net.rssiAvg = updateRssiAvg(net.rssiAvg, rssi);
            net.lastSeen = now;
            net.beaconCount++;
            if (net.lastBeaconSeen > 0) {
                uint32_t delta = now - net.lastBeaconSeen;
                if (delta > 0 && delta < BEACON_INTERVAL_MAX_MS) {
                    if (net.beaconIntervalEmaMs == 0) {
                        net.beaconIntervalEmaMs = (uint16_t)delta;
                    } else {
                        uint32_t blended = (uint32_t)net.beaconIntervalEmaMs * 7 + delta;
                        net.beaconIntervalEmaMs = (uint16_t)(blended / 8);
                    }
                }
            }
            net.lastBeaconSeen = now;
            net.hasPMF |= pmf.required;
        }
        taskEXIT_CRITICAL(&vectorMux);
    }
}

static void processProbeResponse(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (len < 36) return;
    
    const uint8_t* bssid = payload + 16;
    uint32_t now = millis();

    // Parse SSID from IE (probe responses can reveal hidden SSIDs)
    char ssidBuf[33] = {0};
    bool ssidFound = false;
    bool ssidAllNull = true;
    uint16_t offset = 36;
    while (offset + 2 < len) {
        uint8_t id = payload[offset];
        uint8_t ieLen = payload[offset + 1];
        
        if (offset + 2 + ieLen > len) break;
        
        if (id == 0 && ieLen > 0 && ieLen <= 32) {
            memcpy(ssidBuf, payload + offset + 2, ieLen);
            ssidBuf[ieLen] = 0;
            ssidFound = true;
            
            for (uint8_t i = 0; i < ieLen; i++) {
                if (ssidBuf[i] != 0) { ssidAllNull = false; break; }
            }
            break;
        }
        
        offset += 2 + ieLen;
    }
    
    // [BUG5 FIX] Do lookup inside critical section to prevent TOCTOU race
    // cleanupStaleNetworks() can modify vector between lookup and use
    taskENTER_CRITICAL(&vectorMux);
    int idx = findNetworkInternal(bssid);
    
    if (idx < 0) {
        taskEXIT_CRITICAL(&vectorMux);
        if (ssidFound && !ssidAllNull) {
            revealSsidIfKnown(bssid, ssidBuf);
        }
        return;
    }
    
    // idx is valid and we hold the lock - safe to use
    if ((networks[idx].ssid[0] == 0 || networks[idx].isHidden) && ssidFound && !ssidAllNull) {
        memcpy(networks[idx].ssid, ssidBuf, 33);
        networks[idx].isHidden = false;
    }
    
    networks[idx].rssi = rssi;
    networks[idx].rssiAvg = updateRssiAvg(networks[idx].rssiAvg, rssi);
    networks[idx].lastSeen = now;
    taskEXIT_CRITICAL(&vectorMux);
}

static void processAssocRequest(const uint8_t* payload, uint16_t len, bool isReassoc) {
    if (len < 36) return;
    
    const uint8_t* bssid = payload + 16;
    uint16_t fixedLen = isReassoc ? 10 : 4;
    uint16_t offset = 24 + fixedLen;
    if (offset + 2 > len) return;
    
    // Parse SSID IE from tagged parameters
    char ssidBuf[33] = {0};
    bool ssidFound = false;
    bool ssidAllNull = true;
    while (offset + 2 < len) {
        uint8_t id = payload[offset];
        uint8_t ieLen = payload[offset + 1];
        if (offset + 2 + ieLen > len) break;
        
        if (id == 0 && ieLen > 0 && ieLen <= 32) {
            memcpy(ssidBuf, payload + offset + 2, ieLen);
            ssidBuf[ieLen] = 0;
            ssidFound = true;
            for (uint8_t i = 0; i < ieLen; i++) {
                if (ssidBuf[i] != 0) { ssidAllNull = false; break; }
            }
            break;
        }
        offset += 2 + ieLen;
    }
    
    if (ssidFound && !ssidAllNull) {
        revealSsidIfKnown(bssid, ssidBuf);
    }
}

static void markDataActivity(const uint8_t* bssid, const uint8_t* clientMac) {
    taskENTER_CRITICAL(&vectorMux);

    int idx = findNetworkInternal(bssid);
    if (idx >= 0 && idx < (int)networks.size()) {
        DetectedNetwork& net = networks[idx];
        net.lastDataSeen = millis();
        if (clientMac) {
            uint8_t bit = clientHashIndex(clientMac);
            if (bit < 64) {
                net.clientBitset |= (1ULL << bit);
            } else {
                net.clientBitsetHigh |= (1ULL << (bit - 64));
            }
        }
    }

    taskEXIT_CRITICAL(&vectorMux);
}

static void processDataFrame(const uint8_t* payload, uint16_t len, int8_t rssi) {
    (void)rssi;
    if (len < 28) return;
    
    uint8_t toDs = (payload[1] & 0x01);
    uint8_t fromDs = (payload[1] & 0x02) >> 1;
    
    const uint8_t* bssid = nullptr;
    const uint8_t* clientMac = nullptr;
    
    if (!toDs && fromDs) {
        bssid = payload + 10;
        clientMac = payload + 4;
    } else if (toDs && !fromDs) {
        bssid = payload + 4;
        clientMac = payload + 10;
    }
    
    if (bssid && clientMac) {
        if ((clientMac[0] & 0x01) == 0) {
            markDataActivity(bssid, clientMac);
        }
    }
}

static void promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!buf) return;
    if (!running || paused) return;
    if (busy) {
        PacketCallback cb = modeCallback.load(std::memory_order_relaxed);
        if (cb) {
            cb((wifi_promiscuous_pkt_t*)buf, type);
        }
        return;
    }
    
    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint16_t len = pkt->rx_ctrl.sig_len;
    int8_t rssi = pkt->rx_ctrl.rssi;
    
    // ESP32 adds 4 ghost bytes
    if (len > 4) len -= 4;
    if (len < 24) return;
    
    packetCount.fetch_add(1, std::memory_order_relaxed);
    
    const uint8_t* payload = pkt->payload;
    uint8_t frameType    = (payload[0] >> 2) & 0x03;
    uint8_t frameSubtype = (payload[0] >> 4) & 0x0F;

    // Passive Flock/Raven detection on every frame (cheap OUI check; siren and
    // SD logging are deferred to drainFlockHits() in update()).
    {
        flockdet::Detection fd = g_flockDet.inspectWifiFrame(payload, len, rssi, currentChannel);
        if (fd.hit()) {
            flockMatchCount.fetch_add(1, std::memory_order_relaxed);
            enqueueFlockHit(fd);
        }
    }

    // SQUEAL ALERT: passive 802.11 attack counters (deauth/disassoc/beacon/probe
    // floods). addr2 (payload+10) is the transmitter. Per-second roll-up + alert
    // happen in serviceFlockAlerts() on the main loop.
    g_attackMon.onFrame(frameType, frameSubtype, (len >= 16) ? payload + 10 : nullptr);
    
    // Basic network tracking (always happens)
    switch (type) {
        case WIFI_PKT_MGMT:
            if (frameSubtype == 0x08) {  // Beacon
                processBeacon(payload, len, rssi);
            } else if (frameSubtype == 0x05) {  // Probe Response
                processProbeResponse(payload, len, rssi);
            } else if (frameSubtype == 0x00) {  // Assoc Request
                processAssocRequest(payload, len, false);
            } else if (frameSubtype == 0x02) {  // Reassoc Request
                processAssocRequest(payload, len, true);
            }
            break;
            
        case WIFI_PKT_DATA:
            processDataFrame(payload, len, rssi);
            break;
            
        default:
            break;
    }
    
    // Mode-specific callback (for EAPOL capture, PCAP logging, etc.)
    PacketCallback cb = modeCallback.load(std::memory_order_relaxed);
    if (cb) {
        cb(pkt, type);
    }
}

static void processDeferredEvents() {
    const uint8_t kMaxAddsPerUpdate = 8;
    uint8_t processed = 0;
    DetectedNetwork pending = {};
    
    while (processed < kMaxAddsPerUpdate && dequeuePendingNetwork(pending)) {
        // Apply any deferred SSID reveal before adding
        applyPendingSsid(pending);
        
        // With reserve(MAX_RECON_NETWORKS) at init and no shrink_to_fit,
        // push_back never allocates. At capacity, evict weakest instead of growing.
        bool hasCapacity = networks.size() < networks.capacity();

        bool inserted = false;
        bool replaced = false;
        if (hasCapacity) {
            taskENTER_CRITICAL(&vectorMux);
            networks.push_back(pending);  // Safe: capacity pre-reserved at init
            taskEXIT_CRITICAL(&vectorMux);
            inserted = true;
        } else {
            // Vector is full - evict a low-value entry if the new one is better
            uint32_t now = millis();
            int pendingScore = computeRetentionScore(pending, now);
            int worstScore = 100000;
            int worstIdx = -1;
            
            taskENTER_CRITICAL(&vectorMux);
            for (size_t i = 0; i < networks.size(); i++) {
                if (networks[i].isTarget) continue;
                int score = computeRetentionScore(networks[i], now);
                if (score < worstScore) {
                    worstScore = score;
                    worstIdx = (int)i;
                }
            }
            if (worstIdx >= 0 && pendingScore > worstScore) {
                networks[worstIdx] = pending;
                replaced = true;
            }
            taskEXIT_CRITICAL(&vectorMux);
        }
        
        if (inserted || replaced) {
            // Notify mode of new network discovery (for XP events)
            // Called OUTSIDE critical section - safe for Mood/XP calls
            if (newNetworkCallback) {
                newNetworkCallback(
                    pending.authmode,
                    pending.isHidden,
                    pending.ssid,
                    pending.rssi,
                    pending.channel
                );
            }
        }
        
        processed++;
    }
}

static void cleanupStaleNetworks() {
    uint32_t now = millis();
    
    // [BUG6 FIX] Single critical section for collect + erase
    // Previously had gap between collect and erase where vector could change
    // erase() doesn't allocate - just shifts elements and decrements size - safe in spinlock
    taskENTER_CRITICAL(&vectorMux);
    
    // Collect stale indices (static to avoid stack/heap allocation)
    static size_t staleIndices[50];
    size_t staleCount = 0;

    for (size_t i = 0; i < networks.size() && staleCount < 50; i++) {
        if (networks[i].lastDataSeen > 0 &&
            now - networks[i].lastDataSeen > CLIENT_BITMAP_RESET_MS) {
            networks[i].clientBitset = 0;
            networks[i].clientBitsetHigh = 0;
        }
        int8_t rssi = (networks[i].rssiAvg != 0) ? networks[i].rssiAvg : networks[i].rssi;
        uint32_t timeout = STALE_TIMEOUT_MS;  // 60s default (strong signal)
        if (rssi < -75) timeout = 120000;     // Weak: 2 min
        else if (rssi < -50) timeout = 90000; // Medium: 1.5 min
        if (now - networks[i].lastSeen > timeout) {
            staleIndices[staleCount++] = i;
        }
    }
    
    // Erase in reverse order to preserve indices
    for (int i = staleCount - 1; i >= 0; i--) {
        // No bounds check needed - indices are valid, no vector modification between collect and erase
        networks.erase(networks.begin() + staleIndices[i]);
    }
    
    taskEXIT_CRITICAL(&vectorMux);
}

// ============================================================================
// Public API Implementation
// ============================================================================

void init() {
    if (initialized) return;
    
    Serial.println("[RECON] Initializing NetworkRecon service...");
    
    networks.clear();
    networks.reserve(MAX_RECON_NETWORKS);  // Full upfront reserve — eliminates growth reallocations
    
    packetCount.store(0, std::memory_order_relaxed);
    currentChannel = 1;
    currentChannelIndex = 0;
    lastHopTime = 0;
    lastCleanupTime = 0;
    running = false;
    paused = false;
    channelLocked.store(false, std::memory_order_relaxed);
    busy = false;
    pendingNetWrite = 0;
    pendingNetRead = 0;
    pendingSsidWrite = 0;
    for (uint8_t i = 0; i < PENDING_SSID_SLOTS; i++) {
        pendingSsids[i].ready.store(false, std::memory_order_relaxed);
    }
    modeCallback.store(nullptr, std::memory_order_relaxed);
    heapStabilized = false;
    
    initialized = true;
    Serial.println("[RECON] Initialized");
}

void start() {
    if (!initialized) init();

    // Re-reserve if freeNetworks() dropped capacity to 0
    if (networks.capacity() == 0) {
        networks.reserve(MAX_RECON_NETWORKS);
    }

    if (running) {
        if (paused) {
            resume();
        }
        return;
    }

    Serial.printf("[RECON] Starting background scan... free=%u largest=%u\n",
                  ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    
    heapLargestAtStart = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    heapStabilized = false;
    startTime = millis();
    pendingNetWrite = 0;
    pendingNetRead = 0;
    pendingSsidWrite = 0;
    for (uint8_t i = 0; i < PENDING_SSID_SLOTS; i++) {
        pendingSsids[i].ready.store(false, std::memory_order_relaxed);
    }
    
    // Handle BLE coexistence
    if (NimBLEDevice::isInitialized()) {
        Serial.println("[RECON] BLE active - deinitializing for WiFi coexistence");
        
        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (pScan && pScan->isScanning()) {
            pScan->stop();
            delay(50);
        }
        
        NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
        if (pAdv && pAdv->isAdvertising()) {
            pAdv->stop();
            delay(50);
        }
        
        NimBLEDevice::deinit(true);
        delay(100);
        yield();  // Let deferred FreeRTOS cleanup tasks coalesce freed BLE memory
        delay(50);

        Serial.printf("[RECON] After BLE deinit: free=%u largest=%u\n",
                      ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }

    // Initialize WiFi
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);
    delay(50);
    
    // Randomize MAC if configured
    if (Config::wifi().randomizeMAC) {
        WSLBypasser::randomizeMAC();
    }
    
    WiFi.disconnect();
    delay(50);
    
    // Set up promiscuous mode
    esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
    esp_wifi_set_promiscuous_filter(nullptr);  // Receive all packet types
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    
    running = true;
    paused = false;
    lastHopTime = millis();
    lastCleanupTime = millis();
    
    Serial.printf("[RECON] Started on channel %d\n", currentChannel);
}

void stop() {
    if (!running) return;
    
    Serial.println("[RECON] Stopping...");
    
    running = false;
    paused = false;
    
    WiFiUtils::stopPromiscuous();
    
    // Don't clear networks - they persist for mode reuse
    
    Serial.printf("[RECON] Stopped. Networks cached: %d\n", networks.size());
}

void freeNetworks() {
    taskENTER_CRITICAL(&vectorMux);
    networks.clear();
    networks.shrink_to_fit();
    taskEXIT_CRITICAL(&vectorMux);
    Serial.println("[RECON] Networks vector freed");
}

void pause() {
    if (!running || paused) return;
    
    Serial.println("[RECON] Pausing promiscuous mode...");
    
    paused = true;
    
    // [BUG4 FIX] Save and clear channel lock - will restore on resume if mode still active
    channelLockedBeforePause = channelLocked.load(std::memory_order_acquire);
    if (channelLockedBeforePause) {
        channelLocked.store(false, std::memory_order_release);
        Serial.println("[RECON] Channel lock suspended for pause");
    }
    
    // Disable promiscuous but keep WiFi STA active
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    
    Serial.println("[RECON] Paused (WiFi STA still active)");
}

void resume() {
    if (!running || !paused) return;
    
    Serial.println("[RECON] Resuming promiscuous mode...");
    
    // Disconnect from any network before enabling promiscuous mode
    // (WiFi may be connected after TLS operations like WiGLE/WPA-SEC sync)
    WiFi.disconnect();
    delay(50);
    
    // Re-enable promiscuous
    esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
    esp_wifi_set_promiscuous_filter(nullptr);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    
    paused = false;
    lastHopTime = millis();
    
    // [BUG4 FIX] Restore channel lock only if mode callback still registered
    // (If modeCallback is null, no mode owns the lock anymore)
    if (channelLockedBeforePause && modeCallback.load(std::memory_order_acquire) != nullptr) {
        channelLocked.store(true, std::memory_order_release);
        Serial.printf("[RECON] Channel lock restored to %d\n", lockedChannel);
    }
    channelLockedBeforePause = false;
    
    Serial.printf("[RECON] Resumed on channel %d\n", currentChannel);
}

void update() {
    if (!running || paused) return;
    
    uint32_t now = millis();
    
    // Process deferred events from callback
    processDeferredEvents();

    // NOTE: Flock hits are NOT drained here anymore. This update() early-returns
    // whenever NetworkRecon is paused (e.g. WARHOG's active-scan slice) and is
    // tied to NetworkRecon's lifecycle, which left DO NO HAM silent. The drain
    // now runs unconditionally from the global app loop via serviceFlockAlerts()
    // so the siren fires in every sniffing mode.

    // Channel hopping
    uint32_t hopInterval = getHopIntervalMsInternal();
    if (!channelLocked.load(std::memory_order_acquire) && now - lastHopTime > hopInterval) {
        hopChannel();
        lastHopTime = now;
    }
    
    // Periodic cleanup
    if (now - lastCleanupTime > CLEANUP_INTERVAL_MS) {
        cleanupStaleNetworks();
        lastCleanupTime = now;

        // NOTE: No shrink_to_fit here. The reserve(MAX_RECON_NETWORKS) at init
        // is permanent during operation. Shrinking would force TLSF to find a
        // new differently-sized block on next growth, creating fragmentation.
        // Capacity is only released in freeNetworks() on mode exit.
    }
    
    // Check heap stabilization
    if (!heapStabilized) {
        size_t currentLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        if (currentLargest > HeapPolicy::kHeapStableThreshold) {
            heapStabilized = true;
            Serial.printf("[RECON] Heap stabilized in %ums: largest=%u (was %u)\n",
                          now - startTime, currentLargest, heapLargestAtStart);
        }
    }
}

bool isRunning() {
    return running && !paused;
}

bool isPaused() {
    return running && paused;
}

bool isHeapStable() {
    return heapStabilized;
}

uint8_t getCurrentChannel() {
    return currentChannel;
}

uint32_t getHopIntervalMs() {
    return getHopIntervalMsInternal();
}

void setHopIntervalOverride(uint32_t intervalMs) {
    if (intervalMs == 0) {
        hopIntervalOverrideMs.store(0);
        return;
    }
    if (intervalMs < 50) intervalMs = 50;
    if (intervalMs > 2000) intervalMs = 2000;
    hopIntervalOverrideMs.store(intervalMs);
}

void clearHopIntervalOverride() {
    hopIntervalOverrideMs.store(0);
}

uint32_t getPacketCount() {
    return packetCount.load(std::memory_order_relaxed);
}

uint8_t estimateClientCount(const DetectedNetwork& net) {
    return (uint8_t)(__builtin_popcountll(net.clientBitset) +
                     __builtin_popcountll(net.clientBitsetHigh));
}

static inline uint8_t scoreRssi(int8_t rssi) {
    if (rssi <= -95) return 0;
    if (rssi >= -30) return 60;
    return (uint8_t)(((int)rssi + 95) * 60 / 65);
}

static inline uint8_t scoreRecency(uint32_t ageMs) {
    if (ageMs <= 2000) return 20;
    if (ageMs <= 5000) return 12;
    if (ageMs <= 15000) return 5;
    return 0;
}

static inline uint8_t scoreActivity(uint32_t ageMs) {
    if (ageMs <= 3000) return 20;
    if (ageMs <= 10000) return 10;
    if (ageMs <= 30000) return 5;
    return 0;
}

static inline uint8_t scoreBeaconStability(uint16_t intervalEmaMs) {
    if (intervalEmaMs == 0) return 0;
    if (intervalEmaMs <= 150) return 10;
    if (intervalEmaMs <= 500) return 6;
    if (intervalEmaMs <= 1000) return 3;
    return 0;
}

uint8_t getQualityScore(const DetectedNetwork& net) {
    uint32_t now = millis();
    int8_t rssi = (net.rssiAvg != 0) ? net.rssiAvg : net.rssi;
    uint32_t age = now - net.lastSeen;
    uint32_t dataAge = (net.lastDataSeen > 0) ? (now - net.lastDataSeen) : 0xFFFFFFFFu;
    uint8_t score = 0;
    score += scoreRssi(rssi);
    score += scoreRecency(age);
    if (dataAge != 0xFFFFFFFFu) {
        score += scoreActivity(dataAge);
    }
    score += scoreBeaconStability(net.beaconIntervalEmaMs);
    if (score > 100) score = 100;
    return score;
}

std::vector<DetectedNetwork>& getNetworks() {
    return networks;
}

uint16_t getNetworkCount() {
    return networks.size();
}

bool findNetwork(const uint8_t* bssid, DetectedNetwork* out) {
    taskENTER_CRITICAL(&vectorMux);
    int idx = findNetworkInternal(bssid);
    bool found = (idx >= 0 && idx < (int)networks.size());
    if (found && out) {
        // Copy to caller's buffer while holding lock - pointer becomes invalid after unlock
        *out = networks[idx];
    }
    taskEXIT_CRITICAL(&vectorMux);
    return found;
}

int findNetworkIndex(const uint8_t* bssid) {
    taskENTER_CRITICAL(&vectorMux);
    int idx = findNetworkInternal(bssid);
    taskEXIT_CRITICAL(&vectorMux);
    return idx;
}

void lockChannel(uint8_t channel) {
    if (channel < 1 || channel > 14) return;

    lockedChannel = channel;
    currentChannel = channel;
    channelLocked.store(true, std::memory_order_release);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    
    Serial.printf("[RECON] Channel locked to %d\n", channel);
}

void unlockChannel() {
    channelLocked.store(false, std::memory_order_release);
    Serial.println("[RECON] Channel unlocked, resuming hopping");
}

bool isChannelLocked() {
    return channelLocked.load(std::memory_order_acquire);
}

void setChannel(uint8_t channel) {
    if (channel < 1 || channel > 14) return;
    currentChannel = channel;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

// Drain any Flock/Raven hits and fire the alarm + SD log. Called every frame
// from the global app loop so it runs in EVERY mode, independent of whether
// NetworkRecon::update() ran (it early-returns when paused). Cheap when idle:
// a single relaxed atomic compare when the ring is empty.
void serviceFlockAlerts() {
    drainFlockHits();

    // SQUEAL ALERT: roll the attack window and squeal (rate-limited) when the
    // air turns hostile. Mode-independent, same as the flock drain.
    uint32_t nowA = millis();
    attackdet::AttackType atk = g_attackMon.tick(nowA);
    if (atk != attackdet::AttackType::None) {
        g_lastAttack = atk;
        if (nowA - g_lastAttackAlertMs >= 5000) {   // don't squeal every second
            g_lastAttackAlertMs = nowA;
            const attackdet::AttackStats& s = g_attackMon.stats();
            char toast[64];
            snprintf(toast, sizeof(toast), "SQUEAL: %s\n%02X%02X d%u b%u",
                     attackdet::attackLabel(atk), s.lastBssid[4], s.lastBssid[5],
                     (unsigned)s.deauthPerSec, (unsigned)s.beaconPerSec);
            Display::showToast(toast);
            SFX::play(SFX::PIG_SQUEAL);
            // DEFENSIVE catch: an active attack. Gated by a longer cooldown so a
            // sustained flood can't farm XP (there's no per-device dedup for a
            // flood). ATTACK_XP_COOLDOWN_MS is separate from the 5s toast gate.
            static const uint32_t ATTACK_XP_COOLDOWN_MS = 30000;
            static uint32_t lastAttackXpMs = 0;
            if (lastAttackXpMs == 0 || nowA - lastAttackXpMs >= ATTACK_XP_COOLDOWN_MS) {
                lastAttackXpMs = nowA;
                XP::addXP(XP_DEFENSE_HIT);
                Mood::onDefensiveCatch();
            }
        }
    }

    // FAKE BACON: drain AP observations and flag evil twins (security-mismatch
    // clones of a known SSID). Table + alert live here on the main loop.
    while (apObsRead.load(std::memory_order_relaxed) != apObsWrite.load(std::memory_order_acquire)) {
        uint8_t r = apObsRead.load(std::memory_order_relaxed);
        ApObs obs = apObsRing[r];
        apObsRead.store((uint8_t)((r + 1) % AP_OBS_SLOTS), std::memory_order_release);

        eviltwin::TwinHit twin;
        eviltwin::Sec sec = obs.open ? eviltwin::Sec::Open : eviltwin::Sec::Encrypted;
        if (g_evilTwin.observe(obs.ssid, obs.bssid, sec, twin)) {
            g_evilTwinCount = g_evilTwin.twinCount();
            char toast[80];
            snprintf(toast, sizeof(toast), "FAKE BACON!\n%s NOW %s\n%02X%02X vs %02X%02X",
                     twin.ssid, twin.impostorSec == eviltwin::Sec::Open ? "OPEN" : "ENC",
                     twin.baselineBssid[4], twin.baselineBssid[5],
                     twin.impostorBssid[4], twin.impostorBssid[5]);
            Display::showToast(toast);
            SFX::play(SFX::PIG_ALARM);
            // DEFENSIVE catch: a new evil twin (flagged once per SSID) — XP + fatten.
            XP::addXP(XP_DEFENSE_HIT);
            Mood::onDefensiveCatch();
        }
    }

#if FLOCK_DEBUG_COUNTERS
    // [TEMP DEBUG — strip once DNH confirmed] print the pipeline counters ~3s,
    // and only when something changed, so the log isn't spammed.
    static uint32_t lastPrint = 0;
    static uint32_t lastMatch = 0, lastEnq = 0, lastDrain = 0, lastAlert = 0;
    uint32_t now = millis();
    if (now - lastPrint >= 3000) {
        uint32_t m = flockMatchCount.load(std::memory_order_relaxed);
        uint32_t e = flockEnqueueCount.load(std::memory_order_relaxed);
        uint32_t d = flockDrainCount.load(std::memory_order_relaxed);
        uint32_t a = flockAlertCount.load(std::memory_order_relaxed);
        if (m != lastMatch || e != lastEnq || d != lastDrain || a != lastAlert) {
            Serial.printf("[FLOCK-DBG] match=%u enqueue=%u drain=%u alert=%u recon=%d/%s pkts=%u\n",
                          m, e, d, a, (int)running, paused ? "paused" : "run",
                          (unsigned)packetCount.load(std::memory_order_relaxed));
            lastMatch = m; lastEnq = e; lastDrain = d; lastAlert = a;
        }
        lastPrint = now;
    }
#endif
}

const attackdet::AttackStats& getAttackStats() {
    return g_attackMon.stats();
}

attackdet::AttackType getLastAttack() {
    return g_lastAttack;
}

void clearLastAttack() {
    g_lastAttack = attackdet::AttackType::None;
}

uint8_t getEvilTwinCount() {
    return g_evilTwinCount;
}

uint32_t getFlockAlertCount() {
    return flockAlertCount.load(std::memory_order_relaxed);
}

// Run the passive defensive inspectors (flock + attack + evil-twin) on one raw
// 802.11 frame, WITHOUT the full NetworkRecon network-tracking engine. GUARD HOG
// calls this from its own short promiscuous slice so the WiFi-radio detectors
// contribute to the fused score without tearing BLE down. Feeds the same static
// monitors + rings that serviceFlockAlerts() drains on the main loop.
void inspectDefenseFrame(const uint8_t* payload, uint16_t len, int8_t rssi, uint8_t channel) {
    if (!payload || len < 24) return;
    uint8_t fType = (payload[0] >> 2) & 0x03;
    uint8_t fSub  = (payload[0] >> 4) & 0x0F;

    flockdet::Detection fd = g_flockDet.inspectWifiFrame(payload, len, rssi, channel);
    if (fd.hit()) {
        flockMatchCount.fetch_add(1, std::memory_order_relaxed);
        enqueueFlockHit(fd);
    }

    g_attackMon.onFrame(fType, fSub, (len >= 16) ? payload + 10 : nullptr);

    // Beacon -> evil-twin. Use the capability privacy bit as the open/encrypted
    // proxy (bit 4 of the capabilities field at body offset +10).
    if (fType == 0 && fSub == 0x08 && len >= 38) {
        const uint8_t* bssid = payload + 16;
        uint16_t caps = (uint16_t)payload[34] | ((uint16_t)payload[35] << 8);
        bool open = (caps & 0x0010) == 0;
        char ssid[33] = {0};
        uint16_t off = 36;
        while (off + 2 <= len) {
            uint8_t id = payload[off];
            uint8_t ieLen = payload[off + 1];
            if ((uint32_t)off + 2 + ieLen > len) break;
            if (id == 0) {
                if (ieLen > 0 && ieLen <= 32) { memcpy(ssid, payload + off + 2, ieLen); ssid[ieLen] = 0; }
                break;
            }
            off += 2 + ieLen;
        }
        if (ssid[0]) enqueueApObs(ssid, bssid, open);
    }
}

void setPacketCallback(PacketCallback callback) {
    modeCallback.store(callback, std::memory_order_release);
}

void setNewNetworkCallback(NewNetworkCallback callback) {
    newNetworkCallback = callback;
}

void enterCritical() {
    taskENTER_CRITICAL(&vectorMux);
}

void exitCritical() {
    taskEXIT_CRITICAL(&vectorMux);
}

} // namespace NetworkRecon
