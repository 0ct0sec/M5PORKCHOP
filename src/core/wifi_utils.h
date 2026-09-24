#pragma once

#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "heap_policy.h"

namespace WiFiUtils {
    enum class TimeSyncStatus : uint8_t {
        OK = 0,
        SKIP_ALREADY_SYNCED,
        SKIP_NOT_CONNECTED,
        SKIP_LOW_RSSI,
        SKIP_LOW_HEAP,
        FAIL_TIMEOUT
    };
    /**
     * @brief Performs a hard reset of the WiFi subsystem
     * @note Does not power off WiFi driver to prevent RX buffer allocation failures
     */
    void hardReset();
    
    /**
     * @brief Performs a soft shutdown of the WiFi subsystem
     * @note Does not power off WiFi driver to prevent RX buffer allocation failures
     */
    void shutdown();
    
    /**
     * @brief Stops promiscuous mode if currently active
     */
    void stopPromiscuous();
    
    /**
     * @brief Ensures TLS memory reserve of specified size is available
     * @param bytes Size of the memory reserve to ensure
     * @return true if successful, false otherwise
     */
    bool ensureTlsReserve(size_t bytes);
    
    /**
     * @brief Restores the TLS memory reserve after use
     * @return true if successful, false otherwise
     */
    bool restoreTlsReserve();
    
    /**
     * @brief Ensures system time is synchronized via NTP
     * @param timeoutMs Maximum time to wait for sync (default 6000ms)
     * @param force Force resync even if time appears valid (default false)
     * @return true if time is valid, false if sync failed within timeout
     */
    bool ensureTimeSynced(uint32_t timeoutMs = 6000, bool force = false);

    /**
     * @brief Attempt NTP sync when conditions are good (once per boot on success).
     * @note Non-fatal: returns status describing skip/failure.
     */
    TimeSyncStatus maybeSyncTimeForFileTransfer();
    
    /**
     * @brief Conditions heap for TLS operations by releasing fragmented memory
     * 
     * This mimics the "OINK bounce" effect where entering/exiting OINK mode
     * reclaims ~20-30KB of memory by:
     * 1. Deinitializing BLE if active (biggest win - ~20KB)
     * 2. Allocation/deallocation pattern to coalesce heap fragments
     * 
     * Call before TLS operations (WPA-SEC, WiGLE) when contiguous heap is low.
     * 
     * @return Size of largest contiguous block after conditioning
     */
    size_t conditionHeapForTLS();

    /**
     * @brief Short callback-enabled promiscuous brew to coalesce heap
     * @param dwellMs Total dwell time in milliseconds (default 1000ms)
     * @param includeBleCleanup Deinit BLE before brew (default false)
     * @return Size of largest contiguous block after brew
     */
    size_t brewHeap(uint32_t dwellMs = HeapPolicy::kBrewDefaultDwellMs, bool includeBleCleanup = false);
}
