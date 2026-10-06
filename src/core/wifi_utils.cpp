#include "wifi_utils.h"
#include <Arduino.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <WiFi.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <time.h>
#include <NimBLEDevice.h>  // For BLE deinit during heap conditioning
#include "heap_health.h"
#include "heap_policy.h"
#include "heap_gates.h"
#include "network_recon.h"

namespace WiFiUtils {

static SemaphoreHandle_t reserveMutex = nullptr;
static void* tlsReserve = nullptr;
static size_t tlsReserveSize = 0;
static bool tlsReserveReleased = false;
static SemaphoreHandle_t timeSyncMutex = nullptr;
static uint32_t lastTimeSyncMs = 0;
static bool timeSyncedThisBoot = false;

// Initialize all mutexes safely during setup
static bool initialized = false;
static portMUX_TYPE initMux = portMUX_INITIALIZER_UNLOCKED;

// Heap brewing packet counter and callback
static volatile uint32_t brewPacketCount = 0;
static void brewPromiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    brewPacketCount++;
}

static void ensureInitialized() {
    if (initialized) return;
    portENTER_CRITICAL(&initMux);
    if (!initialized) {
        if (!reserveMutex) reserveMutex = xSemaphoreCreateMutex();
        if (!timeSyncMutex) timeSyncMutex = xSemaphoreCreateMutex();
        initialized = true;
    }
    portEXIT_CRITICAL(&initMux);
}

static bool isTimeValid() {
    time_t now = time(nullptr);
    return now >= 1700000000;  // ~2023-11-14
}

static void ensureNvsReady() {
    // Safe even if already initialised
    (void)nvs_flash_init();
}

void stopPromiscuous() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
}


bool ensureTlsReserve(size_t bytes) {
    ensureInitialized(); // Make sure all mutexes are initialized
    
    if (!reserveMutex) return false;
    
    // Use timeout to prevent indefinite blocking
    if (xSemaphoreTake(reserveMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    size_t target = tlsReserveSize;
    if (bytes > 0) target = bytes;

    if (tlsReserve && !tlsReserveReleased) {
        xSemaphoreGive(reserveMutex);
        return true;
    }

    if (target == 0) {
        xSemaphoreGive(reserveMutex);
        return true;
    }

    tlsReserve = heap_caps_malloc(target, MALLOC_CAP_8BIT);
    bool ok = tlsReserve != nullptr;
    tlsReserveSize = target;
    tlsReserveReleased = !ok;
    xSemaphoreGive(reserveMutex);
    return ok;
}

bool restoreTlsReserve() {
    ensureInitialized(); // Make sure all mutexes are initialized
    
    if (!reserveMutex) return false;
    
    // Use timeout to prevent indefinite blocking
    if (xSemaphoreTake(reserveMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    
    if (!tlsReserveReleased || tlsReserveSize == 0) {
        xSemaphoreGive(reserveMutex);
        return true;
    }
    tlsReserve = heap_caps_malloc(tlsReserveSize, MALLOC_CAP_8BIT);
    bool ok = tlsReserve != nullptr;
    if (ok) {
        tlsReserveReleased = false;
    }
    xSemaphoreGive(reserveMutex);
    return ok;
}

bool ensureTimeSynced(uint32_t timeoutMs, bool force) {
    if (!force && isTimeValid()) {
        return true;
    }

    if (!timeSyncMutex) {
        timeSyncMutex = xSemaphoreCreateMutex();
    }
    if (!timeSyncMutex) return false;

    if (xSemaphoreTake(timeSyncMutex, 0) != pdTRUE) {
        return isTimeValid();
    }

    // Avoid thrashing NTP if we just synced
    if (!force && (millis() - lastTimeSyncMs < 5UL * 60UL * 1000UL) && isTimeValid()) {
        xSemaphoreGive(timeSyncMutex);
        return true;
    }

    // Start SNTP
    configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");

    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (isTimeValid()) {
            lastTimeSyncMs = millis();
            xSemaphoreGive(timeSyncMutex);
            return true;
        }
        delay(100);
        yield();
    }

    xSemaphoreGive(timeSyncMutex);
    return isTimeValid();
}

TimeSyncStatus maybeSyncTimeForFileTransfer() {
    // Respect one successful sync per boot.
    if (timeSyncedThisBoot && isTimeValid()) {
        return TimeSyncStatus::SKIP_ALREADY_SYNCED;
    }

    if (WiFi.status() != WL_CONNECTED) {
        return TimeSyncStatus::SKIP_NOT_CONNECTED;
    }

    int rssi = WiFi.RSSI();
    if (rssi < HeapPolicy::kNtpRssiMinDbm) {
        return TimeSyncStatus::SKIP_LOW_RSSI;
    }

    HeapGates::GateStatus gate = HeapGates::checkGate(
        HeapPolicy::kNtpMinFreeHeap,
        HeapPolicy::kNtpMinContig);
    if (gate.failure != HeapGates::TlsGateFailure::None) {
        return TimeSyncStatus::SKIP_LOW_HEAP;
    }

    uint32_t now = millis();
    if (lastTimeSyncMs != 0 &&
        (now - lastTimeSyncMs) < HeapPolicy::kNtpRetryCooldownMs) {
        return TimeSyncStatus::SKIP_ALREADY_SYNCED;
    }

    bool ok = ensureTimeSynced(HeapPolicy::kNtpTimeoutMs, false);
    lastTimeSyncMs = now;
    if (ok) {
        timeSyncedThisBoot = true;
        return TimeSyncStatus::OK;
    }
    return TimeSyncStatus::FAIL_TIMEOUT;
}

void hardReset() {
    stopPromiscuous();

    // IMPORTANT:
    // Do NOT power WiFi off. That triggers esp_wifi_deinit()/esp_wifi_init()
    // and on no-PSRAM builds it often fails to allocate RX buffers:
    //  wifiLowLevelInit(): esp_wifi_init 257
    WiFi.persistent(false);
    WiFi.setSleep(false);

    // Keep STA mode enabled (driver stays alive)
    WiFi.mode(WIFI_STA);

    // THIS IS THE FIX:
    // disconnect(wifioff=false, eraseap=true)
    // wifioff=true causes driver teardown ? RX buffer allocation failure later
    WiFi.disconnect(false, true);

    delay(HeapPolicy::kWiFiShutdownDelayMs);
    ensureNvsReady();
}

void shutdown() {
    stopPromiscuous();

    // Soft shutdown (no driver teardown)
    WiFi.persistent(false);
    WiFi.disconnect(false, true);
    WiFi.mode(WIFI_STA);

    delay(HeapPolicy::kWiFiShutdownDelayMs);
}

// Cooldown guard for manual conditionHeapForTLS() callers.
// The auto-brew path (maybeAutoConditionHeap) has its own 5-layer protection,
// but manual callers (WiGLE, WPA-SEC, XferServer) bypass those layers.
// This timestamp prevents any caller from brewing more often than the
// policy minimum, protecting against accidental loop patterns.
static uint32_t lastManualConditionMs = 0;

size_t conditionHeapForTLS() {
    // Enforce minimum cooldown between manual conditioning calls
    uint32_t now = millis();
    if (lastManualConditionMs != 0 &&
        (now - lastManualConditionMs) < HeapPolicy::kConditionCooldownMinMs) {
        Serial.printf("[HEAP] conditionHeapForTLS() skipped: cooldown (%us remaining)\n",
                      (unsigned)((HeapPolicy::kConditionCooldownMinMs - (now - lastManualConditionMs)) / 1000));
        return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    }
    lastManualConditionMs = now;

    // "OINK Bounce" effect - mimics the heap conditioning that happens
    // when entering/exiting OINK mode, which reclaims ~20-30KB of memory

    size_t initialLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t initialFree = ESP.getFreeHeap();
    
    Serial.printf("[HEAP] Conditioning for TLS: free=%u largest=%u\n", 
                  initialFree, initialLargest);
    
    // Phase 1: BLE cleanup (the BIG win - ~20-30KB!)
    // If PiggyBlues, PigSync, or any BLE mode was previously used,
    // NimBLE holds 20-30KB of buffers that persist even after stopping
    if (NimBLEDevice::isInitialized()) {
        Serial.println("[HEAP] BLE active - deinitializing to reclaim memory");
        
        // Stop any active BLE operations first
        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (pScan && pScan->isScanning()) {
            pScan->stop();
            delay(HeapPolicy::kBleStopDelayMs);
        }
        
        NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
        if (pAdv && pAdv->isAdvertising()) {
            pAdv->stop();
            delay(HeapPolicy::kBleStopDelayMs);
        }
        
        // Deinit BLE completely (clearAll=true to reset all state)
        NimBLEDevice::deinit(true);
        delay(HeapPolicy::kBleDeinitDelayMs);  // Give BLE stack time to fully shut down
        
        Serial.printf("[HEAP] BLE deinit complete: free=%u largest=%u\n",
                      ESP.getFreeHeap(), 
                      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
    
    // Pause NetworkRecon if running — we override the promiscuous callback below
    bool wasReconRunning = NetworkRecon::isRunning();
    if (wasReconRunning) {
        NetworkRecon::pause();
    }

    // Save WiFi STA connection state — brewing requires disconnect + promiscuous
    bool wasConnected = (WiFi.status() == WL_CONNECTED);

    // Phase 2: "Heap Brewing" - WiFi promiscuous cycle with dwell time
    // The OINK bounce effect requires the WiFi driver's internal task to run
    // for ~2-3 seconds to reorganize its buffers. Key observations:
    //   - WiFi.mode(STA) drops heap by ~35KB (driver init)
    //   - First few seconds: heap stays low (~24KB free)
    //   - After 2-3s: WiFi internal task consolidates → heap jumps to ~82KB
    //   - shutdown() doesn't help if we exit too early
    //
    // CRITICAL: Must set promiscuous callback and filter like OINK does!
    // The driver only reorganizes when actually receiving packets.
    
    const uint32_t dwellMs = HeapPolicy::kConditioningDwellMs;
    Serial.printf("[HEAP] Phase 2: WiFi promiscuous brewing (%ums)...\n",
                  (unsigned)dwellMs);
    uint32_t brewStart = millis();
    brewPacketCount = 0;  // Reset packet counter
    
    // Step 1: Initialize WiFi in STA mode (this allocates driver buffers)
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);
    delay(HeapPolicy::kWiFiModeDelayMs);
    
    Serial.printf("[HEAP] After WiFi.mode(STA): free=%u largest=%u\n",
                  ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    
    // Step 2: Enable promiscuous mode WITH CALLBACK (like OINK does!)
    WiFi.disconnect();
    delay(HeapPolicy::kWiFiDisconnectDelayMs);
    esp_wifi_set_promiscuous_rx_cb(brewPromiscuousCallback);  // ← Critical: set callback!
    esp_wifi_set_promiscuous_filter(nullptr);                  // ← Critical: receive all packets!
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    
    Serial.printf("[HEAP] After promiscuous(true)+callback: free=%u largest=%u\n",
                  ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    
    // Step 3: Dwell time with channel hopping - THIS IS THE KEY
    // WHY THIS WORKS (TLSF allocator theory):
    // The ESP-IDF WiFi task allocates temporary RX/TX buffers per-packet from the
    // same TLSF heap pool. Each alloc/free cycle triggers TLSF's O(1) immediate
    // coalescing — adjacent freed blocks merge automatically (Masmano et al. 2004).
    // Channel hopping ensures packets arrive on each channel, driving the WiFi
    // task's internal alloc/free churn. After 2-3 seconds, this churn consolidates
    // scattered free blocks near WiFi driver allocations into larger contiguous
    // regions. The net effect is that free blocks adjacent to the WiFi driver's
    // permanent buffers coalesce, recovering contiguous space for TLS (35KB+).
    // See heap_research.md for Robson bounds proving this is necessary.
    const uint8_t channels[] = {1, 6, 11, 2, 7, 12, 3, 8, 13, 4, 9, 5, 10};
    const uint32_t stepMs = HeapPolicy::kConditioningStepMs;
    uint32_t steps = (dwellMs + stepMs - 1) / stepMs;
    if (steps < 1) steps = 1;
    for (uint32_t i = 0; i < steps; i++) {
        esp_wifi_set_channel(channels[i % 13], WIFI_SECOND_CHAN_NONE);
        delay(stepMs);
        yield();  // Let background tasks run
        
        // Check if heap has improved (early exit if already good)
        size_t currentLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        uint32_t elapsedMs = (i + 1) * stepMs;
        if (elapsedMs > HeapPolicy::kConditioningWarmupMs &&
            currentLargest > HeapPolicy::kHeapStableThreshold) {
            Serial.printf("[HEAP] Early exit at %dms - heap stabilized (pkts=%u)\n", 
                          (int)elapsedMs, brewPacketCount);
            break;
        }
        
        // Log progress every second
        if (HeapPolicy::kConditioningLogIntervalMs > 0 &&
            (elapsedMs % HeapPolicy::kConditioningLogIntervalMs) == 0) {
            Serial.printf("[HEAP] Brew %ds: free=%u largest=%u pkts=%u\n",
                          (unsigned)(elapsedMs / 1000),
                          ESP.getFreeHeap(), currentLargest, brewPacketCount);
        }
    }
    
    Serial.printf("[HEAP] After brew dwell: free=%u largest=%u pkts=%u\n",
                  ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  brewPacketCount);
    
    // Step 4: Clean shutdown (same as OINK stop)
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    // Preserve AP config (eraseap=false) so callers can reconnect after brewing
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_STA);
    delay(HeapPolicy::kWiFiShutdownDelayMs);
    
    Serial.printf("[HEAP] Brew complete (%ums): free=%u largest=%u\n",
                  millis() - brewStart,
                  ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    
    // Final consolidation delay
    delay(HeapPolicy::kConditioningFinalDelayMs);
    yield();
    
    size_t finalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t finalFree = ESP.getFreeHeap();
    
    int32_t freedBytes = (int32_t)finalFree - (int32_t)initialFree;
    int32_t contiguousGain = (int32_t)finalLargest - (int32_t)initialLargest;
    
    Serial.printf("[HEAP] Conditioning complete: free=%u (%+d) largest=%u (%+d)\n",
                  finalFree, freedBytes, finalLargest, contiguousGain);
    HeapHealth::resetPeaks(true);
    lastManualConditionMs = millis();  // Cooldown starts from brew completion

    // Restore WiFi STA connection if it was active before brewing
    if (wasConnected) {
        Serial.println("[HEAP] Reconnecting WiFi after conditioning...");
        WiFi.reconnect();
        uint32_t reconStart = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - reconStart) < 10000) {
            delay(100);
            yield();
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[HEAP] WiFi reconnected: %s\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.println("[HEAP] WiFi reconnect failed (caller should retry)");
        }
    }

    // Resume NetworkRecon if it was running before conditioning
    if (wasReconRunning) {
        NetworkRecon::resume();
    }

    return finalLargest;
}

// Configurable heap conditioning via WiFi promiscuous mode churn.
// Exploits TLSF's immediate coalescing property: the WiFi task's
// internal alloc/free cycles during packet processing cause adjacent
// free blocks to merge, recovering contiguous heap space.
// BLE cleanup reclaims 20-30KB (NimBLE internal RAM buffers).
// The delay() calls after BLE deinit give FreeRTOS idle task time
// to run deferred cleanup callbacks that free BLE memory.
size_t brewHeap(uint32_t dwellMs, bool includeBleCleanup) {
    size_t initialLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t initialFree = ESP.getFreeHeap();
    Serial.printf("[HEAP] Brew start: free=%u largest=%u dwell=%ums\n",
                  initialFree, initialLargest, (unsigned)dwellMs);

    if (includeBleCleanup && NimBLEDevice::isInitialized()) {
        Serial.println("[HEAP] Brew: BLE active - deinitializing");
        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (pScan && pScan->isScanning()) {
            pScan->stop();
            delay(HeapPolicy::kBleStopDelayMs);
        }
        NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
        if (pAdv && pAdv->isAdvertising()) {
            pAdv->stop();
            delay(HeapPolicy::kBleStopDelayMs);
        }
        NimBLEDevice::deinit(true);
        delay(HeapPolicy::kBleDeinitDelayMs);
    }

    brewPacketCount = 0;
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);
    delay(HeapPolicy::kWiFiModeDelayMs);

    WiFi.disconnect();
    delay(HeapPolicy::kWiFiDisconnectDelayMs);
    esp_wifi_set_promiscuous_rx_cb(brewPromiscuousCallback);
    esp_wifi_set_promiscuous_filter(nullptr);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    const uint8_t channels[] = {1, 6, 11, 2, 7, 12, 3, 8, 13, 4, 9, 5, 10};
    const uint32_t stepMs = HeapPolicy::kConditioningStepMs;
    uint32_t steps = (dwellMs + stepMs - 1) / stepMs;
    if (steps < 1) steps = 1;
    for (uint32_t i = 0; i < steps; i++) {
        esp_wifi_set_channel(channels[i % 13], WIFI_SECOND_CHAN_NONE);
        delay(stepMs);
        yield();

        // Early exit if heap has stabilized (same check as conditionHeapForTLS)
        uint32_t elapsedMs = (i + 1) * stepMs;
        if (elapsedMs > HeapPolicy::kConditioningWarmupMs) {
            size_t currentLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            if (currentLargest > HeapPolicy::kHeapStableThreshold) {
                Serial.printf("[HEAP] Brew early exit at %ums (largest=%u)\n",
                              elapsedMs, (unsigned)currentLargest);
                break;
            }
        }
    }

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    WiFi.disconnect(false, true);
    WiFi.mode(WIFI_STA);
    delay(HeapPolicy::kWiFiShutdownDelayMs);

    size_t finalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t finalFree = ESP.getFreeHeap();
    Serial.printf("[HEAP] Brew complete: free=%u (%+d) largest=%u (%+d) pkts=%u\n",
                  finalFree, (int)(finalFree - initialFree),
                  finalLargest, (int)(finalLargest - initialLargest),
                  brewPacketCount);
    HeapHealth::resetPeaks(true);
    return finalLargest;
}

}  // namespace WiFiUtils
