// Warhog Mode implementation

#include "warhog.h"
#include "../core/config.h"
#include "../core/wsl_bypasser.h"
#include "../core/sdlog.h"
#include "../core/xp.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../ml/features.h"
#include "../ml/inference.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

// Maximum entries in RAM to prevent memory exhaustion
// Each WardrivingEntry ~140 bytes, 500 entries = ~70KB (safe for ESP32-S3 with 320KB DRAM)
// After saving, entries are removed from RAM but BSSID tracked in seenBSSIDs set
static const size_t MAX_ENTRIES = 500;

// Maximum BSSIDs tracked in seenBSSIDs set
// Each std::set node = 24 bytes (8 byte key + 16 byte tree overhead)
// 5000 entries = ~120KB - leaves headroom for other allocations
static const size_t MAX_SEEN_BSSIDS = 5000;

// Heap threshold for emergency cleanup (bytes)
static const size_t HEAP_WARNING_THRESHOLD = 40000;
static const size_t HEAP_CRITICAL_THRESHOLD = 25000;

// SD card retry settings (SD can be busy with other operations)
static const int SD_RETRY_COUNT = 3;
static const int SD_RETRY_DELAY_MS = 10;

// Graceful stop request flag for background scan task
static volatile bool stopRequested = false;

// Helper: Open SD file with retry logic
static File openFileWithRetry(const char* path, const char* mode) {
    File f;
    for (int retry = 0; retry < SD_RETRY_COUNT; retry++) {
        f = SD.open(path, mode);
        if (f) return f;
        delay(SD_RETRY_DELAY_MS);
    }
    return f;  // Returns invalid File if all retries failed
}

// Haversine formula for GPS distance calculation
static double haversineMeters(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;  // Earth radius in meters
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    lat1 = lat1 * M_PI / 180.0;
    lat2 = lat2 * M_PI / 180.0;
    
    double a = sin(dLat / 2) * sin(dLat / 2) +
               cos(lat1) * cos(lat2) * sin(dLon / 2) * sin(dLon / 2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    return R * c;
}

// Distance tracking state
static double lastGPSLat = 0;
static double lastGPSLon = 0;
static uint32_t lastDistanceCheck = 0;

// Static members
bool WarhogMode::running = false;
uint32_t WarhogMode::lastScanTime = 0;
uint32_t WarhogMode::scanInterval = 5000;
std::vector<WardrivingEntry> WarhogMode::entries;
std::set<uint64_t> WarhogMode::seenBSSIDs;
size_t WarhogMode::newCount = 0;
uint32_t WarhogMode::totalNetworks = 0;
uint32_t WarhogMode::openNetworks = 0;
uint32_t WarhogMode::wepNetworks = 0;
uint32_t WarhogMode::wpaNetworks = 0;
uint32_t WarhogMode::savedCount = 0;
String WarhogMode::currentFilename = "";

// Scan state (moved from local static to class static for proper reset)
bool WarhogMode::scanInProgress = false;
uint32_t WarhogMode::scanStartTime = 0;

// Enhanced mode statics
bool WarhogMode::enhancedMode = false;
std::map<uint64_t, WiFiFeatures> WarhogMode::beaconFeatures;
uint32_t WarhogMode::beaconCount = 0;
volatile bool WarhogMode::beaconMapBusy = false;

// Background scan task statics
TaskHandle_t WarhogMode::scanTaskHandle = NULL;
volatile int WarhogMode::scanResult = -2;  // -2 = not started, -1 = running, >=0 = complete

// Periodic ML export
uint32_t WarhogMode::lastMLExport = 0;

// Scan task check: returns true if should abort
static inline bool shouldAbortScan() {
    return stopRequested || !WarhogMode::isRunning();
}

void WarhogMode::init() {
    entries.clear();
    seenBSSIDs.clear();
    newCount = 0;
    totalNetworks = 0;
    openNetworks = 0;
    wepNetworks = 0;
    wpaNetworks = 0;
    savedCount = 0;
    currentFilename = "";
    
    // Check if Enhanced ML mode is enabled
    enhancedMode = (Config::ml().collectionMode == MLCollectionMode::ENHANCED);
    // Guard beacon map in case callback still registered from abnormal shutdown
    beaconMapBusy = true;
    beaconFeatures.clear();
    beaconMapBusy = false;
    beaconCount = 0;
    
    scanInterval = Config::gps().updateInterval * 1000;
    
    Serial.printf("[WARHOG] Initialized (ML Mode: %s)\n", 
                  enhancedMode ? "Enhanced" : "Basic");
}

void WarhogMode::start() {
    if (running) return;
    
    Serial.println("[WARHOG] Starting...");
    
    // Clear previous session data
    entries.clear();
    seenBSSIDs.clear();
    totalNetworks = 0;
    openNetworks = 0;
    wepNetworks = 0;
    wpaNetworks = 0;
    savedCount = 0;
    currentFilename = "";
    // Guard beacon map in case callback still registered from previous session
    beaconMapBusy = true;
    beaconFeatures.clear();
    beaconMapBusy = false;
    beaconCount = 0;
    
    // Reset distance tracking for XP
    lastGPSLat = 0;
    lastGPSLon = 0;
    lastDistanceCheck = 0;
    
    // Reload scan interval from config
    scanInterval = Config::gps().updateInterval * 1000;
    Serial.printf("[WARHOG] Scan interval: %lu ms\n", scanInterval);
    
    // Reset ML export timer
    lastMLExport = millis();
    
    // Reset stop flag for clean start
    stopRequested = false;
    
    // Check if Enhanced ML mode is enabled (might have changed in settings)
    enhancedMode = (Config::ml().collectionMode == MLCollectionMode::ENHANCED);
    
    // Full WiFi reinitialization for clean slate
    WiFi.disconnect(true);  // Disconnect and clear settings
    WiFi.mode(WIFI_OFF);    // Turn off WiFi completely
    delay(200);             // Let it settle
    WiFi.mode(WIFI_STA);    // Station mode for scanning
    
    // Randomize MAC if enabled (stealth)
    if (Config::wifi().randomizeMAC) {
        WSLBypasser::randomizeMAC();
    }
    
    delay(200);             // Let it initialize
    
    // Reset scan state (critical for proper operation after restart)
    scanInProgress = false;
    scanStartTime = 0;
    
    Serial.println("[WARHOG] WiFi initialized in STA mode");
    
    // If Enhanced mode, start promiscuous capture for beacons
    if (enhancedMode) {
        startEnhancedCapture();
    }
    
    // Wake up GPS
    GPS::wake();
    
    running = true;
    lastScanTime = 0;  // Trigger immediate scan
    newCount = 0;
    
    // Set grass speed for wardriving - animation controlled by GPS lock in update()
    Avatar::setGrassSpeed(200);  // Slower than OINK (~5 FPS)
    Avatar::setGrassMoving(GPS::hasFix());  // Start based on current GPS status
    
    Display::setWiFiStatus(true);
    Mood::onWarhogUpdate();  // Show WARHOG phrase on start
    Serial.printf("[WARHOG] Running (ML Mode: %s)\n", 
                  enhancedMode ? "Enhanced" : "Basic");
}

void WarhogMode::stop() {
    if (!running) return;
    
    Serial.println("[WARHOG] Stopping...");
    
    // Signal task to stop gracefully
    stopRequested = true;
    
    // Stop Enhanced mode capture first
    if (enhancedMode) {
        stopEnhancedCapture();
    }
    
    // Wait briefly for background scan to notice stopRequested
    if (scanInProgress && scanTaskHandle != NULL) {
        Serial.println("[WARHOG] Waiting for background scan to finish...");
        // Give task up to 500ms to exit gracefully
        for (int i = 0; i < 10 && scanTaskHandle != NULL; i++) {
            delay(50);
        }
        // Force cleanup if task didn't exit in time
        if (scanTaskHandle != NULL) {
            Serial.println("[WARHOG] Force-cancelling background scan...");
            vTaskDelete(scanTaskHandle);
            scanTaskHandle = NULL;
        }
        // Always clean up scan data
        WiFi.scanDelete();
    }
    scanInProgress = false;
    scanResult = -2;
    
    // Stop grass animation
    Avatar::setGrassMoving(false);
    
    running = false;
    
    // Final save of any entries still in RAM
    if (!entries.empty() || seenBSSIDs.size() > 0) {
        Serial.printf("[WARHOG] Final save - %lu in RAM, %lu total tracked\n", 
                      entries.size(), seenBSSIDs.size());
        saveNewEntries();  // Flush remaining to existing CSV (append mode)
        
        // Auto-export ML training data if Enhanced mode was used
        if (Config::ml().collectionMode == MLCollectionMode::ENHANCED) {
            exportMLTraining("/ml_training.csv");
        }
    }
    
    // Put GPS to sleep if power management enabled
    if (Config::gps().powerSave) {
        GPS::sleep();
    }
    
    Display::setWiFiStatus(false);
    
    // Reset stop flag for next run
    stopRequested = false;
    
    Serial.println("[WARHOG] Stopped");
}

// Background task for WiFi scanning - runs sync scan without blocking main loop
void WarhogMode::scanTask(void* pvParameters) {
    Serial.println("[WARHOG] Scan task starting sync scan...");
    
    // Check for early abort request
    if (shouldAbortScan()) {
        Serial.println("[WARHOG] Scan task: abort requested before start");
        scanResult = -2;
        scanTaskHandle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    // Do full WiFi reset for reliable scanning
    WiFi.scanDelete();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Check abort between WiFi operations
    if (shouldAbortScan()) {
        Serial.println("[WARHOG] Scan task: abort requested during reset");
        scanResult = -2;
        scanTaskHandle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    WiFi.mode(WIFI_STA);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Sync scan - this blocks until complete (which is fine in background task)
    int result = WiFi.scanNetworks(false, true);  // sync, show hidden
    
    Serial.printf("[WARHOG] Scan task got %d networks\n", result);
    
    // Store result for main loop to pick up
    scanResult = result;
    
    // Re-enable promiscuous mode for beacon capture if Enhanced mode
    // Must re-register callback after WiFi reset - need delay for WiFi to be ready
    if (enhancedMode) {
        vTaskDelay(pdMS_TO_TICKS(50));  // Let WiFi stabilize
        
        wifi_promiscuous_filter_t filter = {
            .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
        };
        esp_err_t err1 = esp_wifi_set_promiscuous_filter(&filter);
        esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
        esp_err_t err2 = esp_wifi_set_promiscuous(true);
        
        if (err1 != ESP_OK || err2 != ESP_OK) {
            Serial.printf("[WARHOG] Promisc re-enable failed: filter=%d, enable=%d\n", err1, err2);
        }
    }
    
    // Clean up task handle
    scanTaskHandle = NULL;
    vTaskDelete(NULL);  // Self-delete
}

void WarhogMode::update() {
    if (!running) return;
    
    uint32_t now = millis();
    static uint32_t lastPhraseTime = 0;
    static bool lastGPSState = false;
    static uint32_t lastHeapCheck = 0;
    
    // Periodic heap monitoring (every 30 seconds)
    if (now - lastHeapCheck >= 30000) {
        uint32_t freeHeap = ESP.getFreeHeap();
        Serial.printf("[WARHOG] Heap: %lu free, Entries: %lu, SeenBSSIDs: %lu, BeaconCache: %lu\n",
                      freeHeap, entries.size(), seenBSSIDs.size(), beaconFeatures.size());
        
        if (freeHeap < HEAP_CRITICAL_THRESHOLD) {
            Serial.println("[WARHOG] CRITICAL: Low heap! Emergency cleanup...");
            Display::showToast("Low memory!");
            // Emergency: clear tracking data to prevent crash
            // Guard beacon map from promiscuous callback during clear
            beaconMapBusy = true;
            seenBSSIDs.clear();
            beaconFeatures.clear();
            beaconMapBusy = false;
        } else if (freeHeap < HEAP_WARNING_THRESHOLD) {
            Serial.println("[WARHOG] WARNING: Heap getting low");
        }
        lastHeapCheck = now;
    }
    
    // Update grass animation based on GPS fix status
    bool hasGPSFix = GPS::hasFix();
    if (hasGPSFix != lastGPSState) {
        Avatar::setGrassMoving(hasGPSFix);
        lastGPSState = hasGPSFix;
        Serial.printf("[WARHOG] GPS %s - grass %s\n", 
                      hasGPSFix ? "locked" : "lost", 
                      hasGPSFix ? "moving" : "stopped");
    }
    
    // Fix: Proactive cleanup when GPS lost for extended time
    // Prevents unsaved entries from accumulating indefinitely
    static uint32_t gpsLostTime = 0;
    if (!hasGPSFix) {
        if (gpsLostTime == 0) gpsLostTime = now;
        // If GPS lost for >60 seconds and entries filling up, trim oldest unsaved
        if (now - gpsLostTime > 60000 && entries.size() > MAX_ENTRIES / 2) {
            size_t unsavedCount = 0;
            for (const auto& e : entries) {
                if (!e.saved) unsavedCount++;
            }
            
            if (unsavedCount > MAX_ENTRIES / 4) {  // More than 25% unsaved
                Serial.printf("[WARHOG] GPS lost >60s, trimming %lu old unsaved entries\n", 
                              unsavedCount / 2);
                // Remove oldest unsaved entries (first half of unsaved)
                std::vector<WardrivingEntry> kept;
                kept.reserve(entries.size() - unsavedCount / 2);
                size_t trimCount = 0;
                size_t targetTrim = unsavedCount / 2;
                for (const auto& e : entries) {
                    if (!e.saved && trimCount < targetTrim) {
                        trimCount++;  // Discard this one
                    } else {
                        kept.push_back(e);
                    }
                }
                entries = std::move(kept);
                entries.shrink_to_fit();
                SDLOG("WARHOG", "Trimmed %lu entries, %lu remaining", trimCount, entries.size());
            }
        }
    } else {
        gpsLostTime = 0;  // Reset when GPS returns
    }
    
    // Distance tracking for XP (every 5 seconds when GPS is available)
    if (hasGPSFix && now - lastDistanceCheck >= 5000) {
        GPSData gps = GPS::getData();
        if (lastGPSLat != 0 && lastGPSLon != 0) {
            double distance = haversineMeters(lastGPSLat, lastGPSLon, gps.latitude, gps.longitude);
            // Filter out GPS jitter (<5m) and teleportation (>1km)
            if (distance > 5.0 && distance < 1000.0) {
                XP::addDistance((uint32_t)distance);
            }
        }
        lastGPSLat = gps.latitude;
        lastGPSLon = gps.longitude;
        lastDistanceCheck = now;
    }
    
    // Rotate phrases every 5 seconds when idle
    if (now - lastPhraseTime >= 5000) {
        Mood::onWarhogUpdate();
        lastPhraseTime = now;
    }
    
    // Check if background scan task is complete
    if (scanInProgress) {
        if (scanResult >= 0) {
            // Scan done
            Serial.printf("[WARHOG] Background scan complete: %d networks in %lu ms\n", 
                         scanResult, millis() - scanStartTime);
            scanInProgress = false;
            processScanResults();
            scanResult = -2;  // Reset for next scan
        } else if (scanTaskHandle == NULL && scanResult == -2) {
            // Task ended but no result - something went wrong
            Serial.println("[WARHOG] Background scan task ended unexpectedly");
            scanInProgress = false;
        } else if (now - scanStartTime > 20000) {
            // Timeout after 20 seconds
            Serial.println("[WARHOG] Background scan timeout");
            if (scanTaskHandle != NULL) {
                vTaskDelete(scanTaskHandle);
                scanTaskHandle = NULL;
            }
            scanInProgress = false;
            scanResult = -2;
            WiFi.scanDelete();
        }
        // Still running - just return (UI stays responsive)
        return;
    }
    
    // Start new scan if interval elapsed and not already scanning
    if (now - lastScanTime >= scanInterval) {
        // Check if ML export is due - do it before starting scan
        if (enhancedMode && !entries.empty() && (now - lastMLExport >= ML_EXPORT_INTERVAL)) {
            if (Config::isSDAvailable()) {
                Serial.println("[WARHOG] Periodic ML export (before scan)...");
                exportMLTraining("/ml_training.csv");
                lastMLExport = now;
            }
        }
        performScan();
        lastScanTime = now;
    }
}

void WarhogMode::triggerScan() {
    if (!scanInProgress) {
        performScan();
    }
}

bool WarhogMode::isScanComplete() {
    return !scanInProgress && scanResult >= 0;
}

void WarhogMode::performScan() {
    if (scanInProgress) return;
    if (scanTaskHandle != NULL) return;  // Previous task still running
    
    Serial.println("[WARHOG] Starting background WiFi scan...");
    
    // Temporarily disable promiscuous mode for scanning (conflicts with scan API)
    if (enhancedMode) {
        esp_wifi_set_promiscuous(false);
    }
    
    scanInProgress = true;
    scanStartTime = millis();
    scanResult = -1;  // Running
    
    // Create background task for sync scan
    xTaskCreatePinnedToCore(
        scanTask,           // Function
        "wifiScan",         // Name
        4096,               // Stack size
        NULL,               // Parameters
        1,                  // Priority (low)
        &scanTaskHandle,    // Task handle
        0                   // Run on core 0 (WiFi core)
    );
    
    if (scanTaskHandle == NULL) {
        Serial.println("[WARHOG] Failed to create scan task");
        scanInProgress = false;
        scanResult = -2;
    }
}
void WarhogMode::processScanResults() {
    // Results are already in WiFi library from sync scan
    int n = scanResult;
    
    if (n < 0) {
        Serial.println("[WARHOG] No valid scan results");
        WiFi.scanDelete();
        return;
    }
    
    // Reset count for this scan cycle
    newCount = 0;
    
    // Get current GPS data
    GPSData gpsData = GPS::getData();
    // For wardriving, accept ANY non-zero coordinates (even cached from previous fix)
    // The age check is too strict when GPS outputs valid coords but hasn't updated recently
    bool hasGPS = (gpsData.latitude != 0.0 && gpsData.longitude != 0.0);
    
    SDLOG("WARHOG", "Found %d networks (GPS: %s, lat=%.6f, lon=%.6f, age=%lu)", 
          n, hasGPS ? "yes" : "no", gpsData.latitude, gpsData.longitude, gpsData.age);
    
    // Guard beacon map access from promiscuous callback
    beaconMapBusy = true;
    
    size_t previousCount = entries.size();
    
    // Use Arduino WiFi library accessors (ESP-IDF buffer is already consumed by WiFi.scanNetworks)
    for (int i = 0; i < n; i++) {
        // Get BSSID as uint8_t array
        uint8_t* bssidPtr = WiFi.BSSID(i);
        if (!bssidPtr) continue;
        
        uint64_t bssidKey = bssidToKey(bssidPtr);
        
        // First check if we've already saved this BSSID (it may have been cleared from entries)
        if (seenBSSIDs.count(bssidKey) > 0) {
            // Already processed and saved, skip
            continue;
        }
        
        int idx = findEntry(bssidPtr);
        
        if (idx < 0) {
            // Check memory limit before adding - if full, just skip (data already saved)
            if (entries.size() >= MAX_ENTRIES) {
                // Don't clear! Just skip adding new ones until some are saved and compacted
                Serial.println("[WARHOG] Max entries in RAM, saving to free space...");
                saveNewEntries();
                compactSavedEntries();  // Remove saved entries from RAM
                
                // If still full after compact, skip this network
                if (entries.size() >= MAX_ENTRIES) {
                    continue;
                }
            }
            
            // New network - use Arduino WiFi accessors
            WardrivingEntry entry = {0};
            memcpy(entry.bssid, bssidPtr, 6);
            String ssid = WiFi.SSID(i);
            strncpy(entry.ssid, ssid.c_str(), 32);
            entry.ssid[32] = '\0';
            entry.rssi = WiFi.RSSI(i);
            entry.channel = WiFi.channel(i);
            entry.authmode = WiFi.encryptionType(i);
            entry.timestamp = millis();
            entry.saved = false;
            entry.label = 0;  // Unknown - user can label later
            
            // Extract ML features - Enhanced mode uses beacon-captured features
            if (enhancedMode) {
                auto it = beaconFeatures.find(bssidKey);
                if (it != beaconFeatures.end()) {
                    // Use beacon-extracted features (full IE parsing, WPS, vendor IEs, etc.)
                    entry.features = it->second;
                    // Update RSSI from scan (may be fresher)
                    entry.features.rssi = entry.rssi;
                    entry.features.snr = (float)(entry.rssi - entry.features.noise);
                } else {
                    // No beacon captured yet, use basic features
                    entry.features = FeatureExtractor::extractBasic(
                        entry.rssi, entry.channel, entry.authmode);
                }
            } else {
                // Basic mode: use basic features
                entry.features = FeatureExtractor::extractBasic(
                    entry.rssi, entry.channel, entry.authmode);
            }
            
            if (hasGPS) {
                entry.latitude = gpsData.latitude;
                entry.longitude = gpsData.longitude;
                entry.altitude = gpsData.altitude;
            }
            
            entries.push_back(entry);
            totalNetworks++;
            
            // Track auth types and award appropriate XP
            switch (entry.authmode) {
                case WIFI_AUTH_OPEN:
                    openNetworks++;
                    XP::addXP(XPEvent::NETWORK_OPEN);
                    break;
                case WIFI_AUTH_WEP:
                    wepNetworks++;
                    XP::addXP(XPEvent::NETWORK_WEP);  // Rare find!
                    break;
                case WIFI_AUTH_WPA3_PSK:
                case WIFI_AUTH_WPA2_WPA3_PSK:
                    wpaNetworks++;
                    XP::addXP(XPEvent::NETWORK_WPA3);
                    break;
                default:
                    wpaNetworks++;
                    XP::addXP(XPEvent::NETWORK_FOUND);
                    break;
            }
            
            Serial.printf("[WARHOG] New: %s (ch%d, %s)\n",
                         entry.ssid, entry.channel, 
                         authModeToString(entry.authmode).c_str());
        } else {
            // Update existing - maybe update GPS if we have better fix
            int8_t currentRssi = WiFi.RSSI(i);
            if (hasGPS && (entries[idx].latitude == 0 || 
                          currentRssi > entries[idx].rssi)) {
                // Track if this was missing coords (will need save)
                bool wasMissingCoords = (entries[idx].latitude == 0);
                
                entries[idx].latitude = gpsData.latitude;
                entries[idx].longitude = gpsData.longitude;
                entries[idx].altitude = gpsData.altitude;
                entries[idx].rssi = currentRssi;
                
                // Update features - Enhanced mode may have better data now
                if (enhancedMode) {
                    uint64_t key = bssidToKey(bssidPtr);
                    auto it = beaconFeatures.find(key);
                    if (it != beaconFeatures.end()) {
                        entries[idx].features = it->second;
                        entries[idx].features.rssi = currentRssi;
                        entries[idx].features.snr = (float)(currentRssi - entries[idx].features.noise);
                    }
                }
                
                // If entry now has coords but wasn't saved yet, count as needing save
                if (wasMissingCoords && !entries[idx].saved) {
                    newCount++;
                }
            }
        }
    }
    
    // Release beacon map guard
    beaconMapBusy = false;
    
    // Add newly discovered networks to count (entries that got coords are already counted above)
    newCount += entries.size() - previousCount;
    
    if (newCount > 0) {
        SDLOG("WARHOG", "newCount=%lu, calling save (hasGPS=%d, SD=%d)", 
              newCount, hasGPS ? 1 : 0, Config::isSDAvailable() ? 1 : 0);
        
        // Use WARHOG-specific phrases for found networks
        Mood::onWarhogFound(nullptr, 0);
        
        // Auto-save new entries with GPS fix to CSV
        if (hasGPS && Config::isSDAvailable()) {
            saveNewEntries();
        }
    } else {
        // Check if there are unsaved entries with coords that should be saved
        uint32_t unsavedWithCoords = 0;
        for (const auto& e : entries) {
            if (!e.saved && e.latitude != 0 && e.longitude != 0) {
                unsavedWithCoords++;
            }
        }
        if (unsavedWithCoords > 0 && hasGPS && Config::isSDAvailable()) {
            SDLOG("WARHOG", "Found %lu unsaved entries with coords, saving", unsavedWithCoords);
            saveNewEntries();
        }
    }
    
    WiFi.scanDelete();
    
    // Re-enable promiscuous mode for beacon capture if Enhanced mode
    if (enhancedMode) {
        esp_wifi_set_promiscuous(true);
    }
}

// Helper to escape XML special characters
static String escapeXML(const char* str) {
    String result;
    for (int i = 0; str[i] && i < 64; i++) {
        switch (str[i]) {
            case '&':  result += "&amp;"; break;
            case '<':  result += "&lt;"; break;
            case '>':  result += "&gt;"; break;
            case '"':  result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default:   result += str[i]; break;
        }
    }
    return result;
}

// Helper to write CSV-escaped SSID field (quoted, doubles internal quotes, strips control chars)
static void writeCSVField(File& f, const char* ssid) {
    f.print("\"");
    for (int i = 0; i < 32 && ssid[i]; i++) {
        if (ssid[i] == '"') {
            f.print("\"\"");
        } else if (ssid[i] >= 32) {  // Skip control characters (newlines, etc)
            f.print(ssid[i]);
        }
    }
    f.print("\"");
}

void WarhogMode::saveNewEntries() {
    SDLOG("WARHOG", "saveNewEntries called, currentFilename='%s'", currentFilename.c_str());
    
    // Ensure wardriving directory exists
    if (!SD.exists("/wardriving")) {
        if (SD.mkdir("/wardriving")) {
            SDLOG("WARHOG", "Created /wardriving directory");
        } else {
            SDLOG("WARHOG", "Failed to create /wardriving directory");
            Serial.println("[WARHOG] Failed to create /wardriving directory");
            return;
        }
    }
    
    // Create file with unique name on first save
    if (currentFilename.length() == 0) {
        currentFilename = generateFilename("csv");
        SDLOG("WARHOG", "Generated filename: %s", currentFilename.c_str());
        
        // Write CSV header (with retry)
        File f = openFileWithRetry(currentFilename.c_str(), FILE_WRITE);
        if (f) {
            f.println("BSSID,SSID,RSSI,Channel,AuthMode,Latitude,Longitude,Altitude,Timestamp");
            f.close();
            SDLOG("WARHOG", "Created file: %s", currentFilename.c_str());
        } else {
            SDLOG("WARHOG", "Failed to create after retries: %s", currentFilename.c_str());
            return;
        }
    }
    
    // Append unsaved entries that have GPS coordinates (with retry)
    File f = openFileWithRetry(currentFilename.c_str(), FILE_APPEND);
    if (!f) {
        SDLOG("WARHOG", "Failed to append after retries to %s", currentFilename.c_str());
        return;
    }
    
    uint32_t newSaved = 0;
    uint32_t skippedNoCoords = 0;
    uint32_t skippedAlreadySaved = 0;
    for (auto& e : entries) {
        // Only save if not already saved AND has GPS coordinates
        if (!e.saved && e.latitude != 0 && e.longitude != 0) {
            f.printf("%02X:%02X:%02X:%02X:%02X:%02X,",
                    e.bssid[0], e.bssid[1], e.bssid[2],
                    e.bssid[3], e.bssid[4], e.bssid[5]);
            
            // Escape SSID for CSV
            writeCSVField(f, e.ssid);
            f.print(",");
            
            f.printf("%d,%d,%s,%.6f,%.6f,%.1f,%lu\n",
                    e.rssi, e.channel, authModeToString(e.authmode).c_str(),
                    e.latitude, e.longitude, e.altitude, e.timestamp);
            
            e.saved = true;
            // Track BSSID to prevent re-processing (skip if set is full)
            if (seenBSSIDs.size() < MAX_SEEN_BSSIDS) {
                seenBSSIDs.insert(bssidToKey(e.bssid));
            }
            newSaved++;
            savedCount++;
        } else if (e.saved) {
            skippedAlreadySaved++;
        } else {
            skippedNoCoords++;
        }
    }
    
    f.flush();  // Ensure data hits SD immediately (crash protection)
    f.close();
    
    SDLOG("WARHOG", "Saved %lu entries (skipped: %lu no coords, %lu already saved)", 
          newSaved, skippedNoCoords, skippedAlreadySaved);
}

void WarhogMode::compactSavedEntries() {
    // Move all saved entries to seenBSSIDs set (8 bytes each vs 200), free RAM
    // Keep unsaved entries (waiting for GPS fix) in entries vector
    
    std::vector<WardrivingEntry> unsaved;
    unsaved.reserve(entries.size() / 4);  // Estimate ~25% unsaved
    
    for (const auto& e : entries) {
        uint64_t key = bssidToKey(e.bssid);
        if (e.saved) {
            // Already on disk - track BSSID (skip if set is full)
            if (seenBSSIDs.size() < MAX_SEEN_BSSIDS) {
                seenBSSIDs.insert(key);
            }
        } else {
            // Not saved yet (no GPS) - keep in RAM
            unsaved.push_back(e);
        }
    }
    
    size_t freed = entries.size() - unsaved.size();
    entries = std::move(unsaved);
    entries.shrink_to_fit();  // Release unused memory
    
    SDLOG("WARHOG", "Compacted: freed %lu entries, kept %lu unsaved, tracking %lu/%lu BSSIDs",
          freed, entries.size(), seenBSSIDs.size(), (unsigned long)MAX_SEEN_BSSIDS);
}

bool WarhogMode::hasGPSFix() {
    return GPS::hasFix();
}

GPSData WarhogMode::getGPSData() {
    return GPS::getData();
}

bool WarhogMode::exportCSV(const char* path) {
    File f = openFileWithRetry(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[WARHOG] Failed to open %s after retries\n", path);
        return false;
    }
    
    // CSV header
    f.println("BSSID,SSID,RSSI,Channel,AuthMode,Latitude,Longitude,Altitude,Timestamp");
    
    for (const auto& e : entries) {
        f.printf("%02X:%02X:%02X:%02X:%02X:%02X,",
                e.bssid[0], e.bssid[1], e.bssid[2],
                e.bssid[3], e.bssid[4], e.bssid[5]);
        
        // Escape SSID for CSV
        writeCSVField(f, e.ssid);
        f.print(",");
        
        f.printf("%d,%d,%s,%.6f,%.6f,%.1f,%lu\n",
                e.rssi, e.channel, authModeToString(e.authmode).c_str(),
                e.latitude, e.longitude, e.altitude, e.timestamp);
    }
    
    f.close();
    Serial.printf("[WARHOG] Exported %d entries to %s\n", entries.size(), path);
    return true;
}

bool WarhogMode::exportWigle(const char* path) {
    File f = openFileWithRetry(path, FILE_WRITE);
    if (!f) {
        return false;
    }
    
    // Wigle CSV format
    f.println("WigleWifi-1.4,appRelease=porkchop,model=M5Cardputer,release=1.0.0,device=ESP32-S3,display=,board=,brand=M5Stack");
    f.println("MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type");
    
    for (const auto& e : entries) {
        f.printf("%02X:%02X:%02X:%02X:%02X:%02X,",
                e.bssid[0], e.bssid[1], e.bssid[2],
                e.bssid[3], e.bssid[4], e.bssid[5]);
        
        writeCSVField(f, e.ssid);
        f.print(",");
        
        f.printf("%s,", authModeToString(e.authmode).c_str());
        
        // Timestamp from GPS or use current date
        GPSData gps = GPS::getData();
        if (gps.date > 0 && gps.time > 0) {
            // Parse DDMMYY and HHMMSSCC format
            uint8_t day = gps.date / 10000;
            uint8_t month = (gps.date / 100) % 100;
            uint8_t year = gps.date % 100;
            uint8_t hour = gps.time / 1000000;
            uint8_t minute = (gps.time / 10000) % 100;
            uint8_t second = (gps.time / 100) % 100;
            
            // Validate date/time ranges
            if (day > 0 && day <= 31 && month > 0 && month <= 12 && year <= 99 &&
                hour < 24 && minute < 60 && second < 60) {
                f.printf("20%02d-%02d-%02d %02d:%02d:%02d,", year, month, day, hour, minute, second);
            } else {
                f.printf("2025-01-01 00:00:00,");
            }
        } else {
            f.printf("2025-01-01 00:00:00,");
        }
        
        f.printf("%d,%d,%.6f,%.6f,%.1f,10.0,WIFI\n",
                e.channel, e.rssi,
                e.latitude, e.longitude, e.altitude);
    }
    
    f.close();
    Serial.printf("[WARHOG] Wigle export: %d entries to %s\n", entries.size(), path);
    return true;
}

bool WarhogMode::exportKismet(const char* path) {
    // Kismet NetXML format - simplified
    File f = openFileWithRetry(path, FILE_WRITE);
    if (!f) {
        return false;
    }
    
    f.println("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    f.println("<detection-run kismet-version=\"porkchop\">");
    
    for (const auto& e : entries) {
        f.println("<wireless-network>");
        f.printf("<BSSID>%02X:%02X:%02X:%02X:%02X:%02X</BSSID>\n",
                e.bssid[0], e.bssid[1], e.bssid[2],
                e.bssid[3], e.bssid[4], e.bssid[5]);
        f.printf("<SSID>%s</SSID>\n", escapeXML(e.ssid).c_str());
        f.printf("<channel>%d</channel>\n", e.channel);
        f.printf("<encryption>%s</encryption>\n", authModeToString(e.authmode).c_str());
        f.println("<gps-info>");
        f.printf("<lat>%.6f</lat>\n", e.latitude);
        f.printf("<lon>%.6f</lon>\n", e.longitude);
        f.printf("<alt>%.1f</alt>\n", e.altitude);
        f.println("</gps-info>");
        f.println("</wireless-network>");
    }
    
    f.println("</detection-run>");
    f.close();
    Serial.printf("[WARHOG] Kismet export: %d entries to %s\n", entries.size(), path);
    return true;
}

int WarhogMode::findEntry(const uint8_t* bssid) {
    for (size_t i = 0; i < entries.size(); i++) {
        if (memcmp(entries[i].bssid, bssid, 6) == 0) {
            return i;
        }
    }
    return -1;
}

String WarhogMode::authModeToString(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        case WIFI_AUTH_WAPI_PSK: return "WAPI";
        default: return "UNKNOWN";
    }
}

String WarhogMode::generateFilename(const char* ext) {
    // Generate filename with date/time from GPS: YYYYMMDD_HHMMSS
    char buf[64];
    GPSData gps = GPS::getData();
    
    if (gps.date > 0 && gps.time > 0) {
        // date format: DDMMYY, time format: HHMMSSCC (centiseconds)
        uint8_t day = gps.date / 10000;
        uint8_t month = (gps.date / 100) % 100;
        uint8_t year = gps.date % 100;
        uint8_t hour = gps.time / 1000000;
        uint8_t minute = (gps.time / 10000) % 100;
        uint8_t second = (gps.time / 100) % 100;
        
        snprintf(buf, sizeof(buf), "/wardriving/warhog_20%02d%02d%02d_%02d%02d%02d.%s",
                year, month, day, hour, minute, second, ext);
    } else {
        snprintf(buf, sizeof(buf), "/wardriving/warhog_%lu.%s", millis(), ext);
    }
    
    return String(buf);
}

bool WarhogMode::exportMLTraining(const char* path) {
    File f = openFileWithRetry(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[WARHOG] Failed to open ML export after retries: %s\n", path);
        return false;
    }
    
    // CSV header - all 32 feature vector values + label + metadata
    f.print("bssid,ssid,");
    f.print("rssi,noise,snr,channel,secondary_ch,beacon_interval,");
    f.print("capability_lo,capability_hi,has_wps,has_wpa,has_wpa2,has_wpa3,");
    f.print("is_hidden,response_time,beacon_count,beacon_jitter,");
    f.print("responds_probe,probe_response_time,vendor_ie_count,");
    f.print("supported_rates,ht_cap,vht_cap,anomaly_score,");
    f.print("f23,f24,f25,f26,f27,f28,f29,f30,f31,");  // Reserved features
    f.println("label,latitude,longitude");
    
    float featureVec[FEATURE_VECTOR_SIZE];
    
    for (const auto& e : entries) {
        // BSSID
        f.printf("%02X:%02X:%02X:%02X:%02X:%02X,",
                e.bssid[0], e.bssid[1], e.bssid[2],
                e.bssid[3], e.bssid[4], e.bssid[5]);
        
        // SSID (escaped)
        writeCSVField(f, e.ssid);
        f.print(",");
        
        // Convert features to vector
        FeatureExtractor::toFeatureVector(e.features, featureVec);
        
        // Write all 32 feature values
        for (int i = 0; i < FEATURE_VECTOR_SIZE; i++) {
            f.printf("%.4f,", featureVec[i]);
        }
        
        // Label and GPS
        f.printf("%d,%.6f,%.6f\n", e.label, e.latitude, e.longitude);
    }
    
    f.close();
    Serial.printf("[WARHOG] ML training export: %d entries to %s\n", entries.size(), path);
    return true;
}

// Enhanced ML Mode - Promiscuous beacon capture

void WarhogMode::promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    // Only process management frames (beacons, probe responses)
    if (type != WIFI_PKT_MGMT) return;
    
    // Skip if main thread is accessing the map
    if (beaconMapBusy) return;
    
    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* frame = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;
    int8_t rssi = pkt->rx_ctrl.rssi;
    
    if (len < 24) return;
    
    // Frame control field - check if it's a beacon (type=0, subtype=8)
    uint16_t frameControl = frame[0] | (frame[1] << 8);
    uint8_t frameType = (frameControl >> 2) & 0x03;      // Type (bits 2-3)
    uint8_t frameSubtype = (frameControl >> 4) & 0x0F;   // Subtype (bits 4-7)
    
    // Only process beacons (subtype 8) and probe responses (subtype 5)
    if (frameType != 0) return;  // Not management
    if (frameSubtype != 8 && frameSubtype != 5) return;
    
    // BSSID is at offset 16 for beacons/probe responses
    const uint8_t* bssid = frame + 16;
    uint64_t key = bssidToKey(bssid);
    
    // Extract full features from beacon frame
    WiFiFeatures features = FeatureExtractor::extractFromBeacon(frame, len, rssi);
    
    // Store or update features for this BSSID
    // Note: map operations are quick, but we limit to prevent memory issues
    if (beaconFeatures.size() < 500) {  // Max 500 BSSIDs in beacon cache
        auto it = beaconFeatures.find(key);
        if (it != beaconFeatures.end()) {
            // Update beacon count and jitter calculation
            it->second.beaconCount++;
            // Simple jitter: difference from expected interval
            // Real interval would need timestamps, simplified here
        } else {
            features.beaconCount = 1;
            beaconFeatures[key] = features;
        }
        beaconCount++;
    }
}

void WarhogMode::startEnhancedCapture() {
    Serial.println("[WARHOG] Starting Enhanced ML capture (promiscuous mode)");
    
    beaconFeatures.clear();
    beaconCount = 0;
    
    // Configure promiscuous mode filter for management frames only
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
    };
    
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
    esp_wifi_set_promiscuous(true);
    
    Serial.println("[WARHOG] Promiscuous mode enabled for beacon capture");
}

void WarhogMode::stopEnhancedCapture() {
    Serial.println("[WARHOG] Stopping Enhanced ML capture");
    
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    
    Serial.printf("[WARHOG] Captured %d beacons from %d BSSIDs\n", 
                  beaconCount, beaconFeatures.size());
}
