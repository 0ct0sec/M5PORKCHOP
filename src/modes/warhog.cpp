// Warhog Mode implementation - Refactored "GPS as Gate" architecture
// 
// Key changes from original:
// - No entries[] vector - data goes directly to disk
// - No "waiting for GPS" state - either GPS or ML-only
// - Simpler memory management - just seenBSSIDs for duplicate detection
// - Per-network file writes instead of batch saves

#include "warhog.h"
#include "../build_info.h"
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
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

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

// WiGLE file size limit for upload compatibility (400KB - leave room for headers)
// Files larger than this will be rotated to a new file
static const size_t WIGLE_FILE_MAX_SIZE = 400000;

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
std::set<uint64_t> WarhogMode::seenBSSIDs;
uint32_t WarhogMode::totalNetworks = 0;
uint32_t WarhogMode::openNetworks = 0;
uint32_t WarhogMode::wepNetworks = 0;
uint32_t WarhogMode::wpaNetworks = 0;
uint32_t WarhogMode::savedCount = 0;      // Geotagged networks (CSV)
uint32_t WarhogMode::mlOnlyCount = 0;     // Networks without GPS (ML only)
String WarhogMode::currentFilename = "";
String WarhogMode::currentMLFilename = "";
String WarhogMode::currentWigleFilename = "";

// Scan state
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

// Scan task check: returns true if should abort
static inline bool shouldAbortScan() {
    return stopRequested || !WarhogMode::isRunning();
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

void WarhogMode::init() {
    seenBSSIDs.clear();
    totalNetworks = 0;
    openNetworks = 0;
    wepNetworks = 0;
    wpaNetworks = 0;
    savedCount = 0;
    mlOnlyCount = 0;
    currentFilename = "";
    currentMLFilename = "";
    currentWigleFilename = "";
    
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
    seenBSSIDs.clear();
    totalNetworks = 0;
    openNetworks = 0;
    wepNetworks = 0;
    wpaNetworks = 0;
    savedCount = 0;
    mlOnlyCount = 0;
    currentFilename = "";
    currentMLFilename = "";
    currentWigleFilename = "";
    
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
    
    // Set grass speed for wardriving - animation controlled by GPS lock in update()
    Avatar::setGrassSpeed(200);  // Slower than OINK (~5 FPS)
    Avatar::startWindupSlide(108, true);  // Slide to right position (same as sirloin hunt)
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
    
    // Log final statistics
    Serial.printf("[WARHOG] Session complete - Total: %lu, Geotagged: %lu, ML-only: %lu\n",
                  totalNetworks, savedCount, mlOnlyCount);
    
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
        Serial.printf("[WARHOG] Heap: %lu free, SeenBSSIDs: %lu, BeaconCache: %lu\n",
                      freeHeap, seenBSSIDs.size(), beaconFeatures.size());
        
        if (freeHeap < HEAP_CRITICAL_THRESHOLD) {
            Serial.println("[WARHOG] CRITICAL: Low heap! Emergency cleanup...");
            Display::showToast("LOW MEMORY!");
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

// Ensure CSV file exists with header
bool WarhogMode::ensureCSVFileReady() {
    if (currentFilename.length() > 0) return true;
    
    // Ensure wardriving directory exists
    if (!SD.exists("/wardriving")) {
        if (!SD.mkdir("/wardriving")) {
            Serial.println("[WARHOG] Failed to create /wardriving directory");
            return false;
        }
    }
    
    currentFilename = generateFilename("csv");
    
    File f = openFileWithRetry(currentFilename.c_str(), FILE_WRITE);
    if (!f) {
        Serial.printf("[WARHOG] Failed to create CSV: %s\n", currentFilename.c_str());
        currentFilename = "";
        return false;
    }
    
    f.println("BSSID,SSID,RSSI,Channel,AuthMode,Latitude,Longitude,Altitude,Timestamp");
    f.close();
    
    Serial.printf("[WARHOG] Created CSV: %s\n", currentFilename.c_str());
    return true;
}

// Ensure ML file exists with header
bool WarhogMode::ensureMLFileReady() {
    if (currentMLFilename.length() > 0) return true;
    
    // Ensure mldata directory exists
    if (!SD.exists("/mldata")) {
        if (!SD.mkdir("/mldata")) {
            Serial.println("[WARHOG] Failed to create /mldata directory");
            return false;
        }
    }
    
    currentMLFilename = generateFilename("ml.csv");
    // Put ML files in /mldata folder
    currentMLFilename.replace("/wardriving/warhog_", "/mldata/ml_training_");
    
    File f = openFileWithRetry(currentMLFilename.c_str(), FILE_WRITE);
    if (!f) {
        Serial.printf("[WARHOG] Failed to create ML file: %s\n", currentMLFilename.c_str());
        currentMLFilename = "";
        return false;
    }
    
    // CSV header - all 32 feature vector values + label + metadata
    f.print("bssid,ssid,");
    f.print("rssi,noise,snr,channel,secondary_ch,beacon_interval,");
    f.print("capability_lo,capability_hi,has_wps,has_wpa,has_wpa2,has_wpa3,");
    f.print("is_hidden,response_time,beacon_count,beacon_jitter,");
    f.print("responds_probe,probe_response_time,vendor_ie_count,");
    f.print("supported_rates,ht_cap,vht_cap,anomaly_score,");
    f.print("f23,f24,f25,f26,f27,f28,f29,f30,f31,");
    f.println("label,latitude,longitude");
    f.close();
    
    Serial.printf("[WARHOG] Created ML file: %s\n", currentMLFilename.c_str());
    return true;
}

// Append single network to CSV file
void WarhogMode::appendCSVEntry(const uint8_t* bssid, const char* ssid,
                                 int8_t rssi, uint8_t channel, wifi_auth_mode_t auth,
                                 double lat, double lon, double alt) {
    if (!ensureCSVFileReady()) return;
    
    File f = openFileWithRetry(currentFilename.c_str(), FILE_APPEND);
    if (!f) return;
    
    f.printf("%02X:%02X:%02X:%02X:%02X:%02X,",
            bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    writeCSVField(f, ssid);
    f.print(",");
    f.printf("%d,%d,%s,%.6f,%.6f,%.1f,%lu\n",
            rssi, channel, authModeToString(auth).c_str(),
            lat, lon, alt, millis());
    f.close();
}

// Append single network to ML file
void WarhogMode::appendMLEntry(const uint8_t* bssid, const char* ssid,
                                const WiFiFeatures& features, uint8_t label,
                                double lat, double lon) {
    if (!ensureMLFileReady()) return;
    
    File f = openFileWithRetry(currentMLFilename.c_str(), FILE_APPEND);
    if (!f) return;
    
    // BSSID
    f.printf("%02X:%02X:%02X:%02X:%02X:%02X,",
            bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    
    // SSID (escaped)
    writeCSVField(f, ssid);
    f.print(",");
    
    // Convert features to vector and write
    float featureVec[FEATURE_VECTOR_SIZE];
    FeatureExtractor::toFeatureVector(features, featureVec);
    
    for (int i = 0; i < FEATURE_VECTOR_SIZE; i++) {
        f.printf("%.4f,", featureVec[i]);
    }
    
    // Label and GPS
    f.printf("%d,%.6f,%.6f\n", label, lat, lon);
    f.close();
}

// Check if WiGLE file needs rotation due to size
void WarhogMode::checkWigleFileRotation() {
    if (currentWigleFilename.length() == 0) return;
    
    File f = SD.open(currentWigleFilename.c_str(), FILE_READ);
    if (!f) return;
    
    size_t fileSize = f.size();
    f.close();
    
    if (fileSize >= WIGLE_FILE_MAX_SIZE) {
        Serial.printf("[WARHOG] WiGLE file rotated at %d bytes\n", fileSize);
        currentWigleFilename = "";  // Force new file creation on next append
    }
}

// Ensure WiGLE file exists with header
bool WarhogMode::ensureWigleFileReady() {
    // Check if current file needs rotation
    checkWigleFileRotation();
    
    if (currentWigleFilename.length() > 0) return true;
    
    // Ensure wardriving directory exists
    if (!SD.exists("/wardriving")) {
        if (!SD.mkdir("/wardriving")) {
            Serial.println("[WARHOG] Failed to create /wardriving directory");
            return false;
        }
    }
    
    currentWigleFilename = generateFilename("wigle.csv");
    
    File f = openFileWithRetry(currentWigleFilename.c_str(), FILE_WRITE);
    if (!f) {
        Serial.printf("[WARHOG] Failed to create WiGLE CSV: %s\n", currentWigleFilename.c_str());
        currentWigleFilename = "";
        return false;
    }
    
    // WiGLE format v1.6 pre-header
    f.print("WigleWifi-1.6,appRelease=");
    #ifdef BUILD_VERSION
    f.print(BUILD_VERSION);
    #else
    f.print("0.1.x");
    #endif
    f.print(",model=M5Cardputer,release=ESP32-S3,device=PORKCHOP,display=240x135,board=m5stack,brand=M5Stack,star=Sol,body=3,subBody=0\n");
    
    // WiGLE format header
    f.println("MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type");
    f.close();
    
    Serial.printf("[WARHOG] Created WiGLE CSV: %s\n", currentWigleFilename.c_str());
    return true;
}

// Convert auth mode to WiGLE capability string format
String WarhogMode::authModeToWigleString(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN:
            return "[ESS]";
        case WIFI_AUTH_WEP:
            return "[WEP][ESS]";
        case WIFI_AUTH_WPA_PSK:
            return "[WPA-PSK-CCMP][ESS]";
        case WIFI_AUTH_WPA2_PSK:
            return "[WPA2-PSK-CCMP][ESS]";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "[WPA-PSK-CCMP+TKIP][WPA2-PSK-CCMP+TKIP][ESS]";
        case WIFI_AUTH_WPA3_PSK:
            return "[WPA3-SAE][ESS]";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "[WPA2-PSK-CCMP][WPA3-SAE][ESS]";
        case WIFI_AUTH_WAPI_PSK:
            return "[WAPI-PSK][ESS]";
        default:
            return "[ESS]";
    }
}

// Append single network to WiGLE file
void WarhogMode::appendWigleEntry(const uint8_t* bssid, const char* ssid,
                                   int8_t rssi, uint8_t channel, wifi_auth_mode_t auth,
                                   double lat, double lon, double alt, double accuracy) {
    if (!ensureWigleFileReady()) return;
    
    File f = openFileWithRetry(currentWigleFilename.c_str(), FILE_APPEND);
    if (!f) return;
    
    // MAC (BSSID with colons)
    f.printf("%02X:%02X:%02X:%02X:%02X:%02X,",
            bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    
    // SSID (escaped)
    writeCSVField(f, ssid);
    f.print(",");
    
    // AuthMode (WiGLE capability string)
    f.print(authModeToWigleString(auth));
    f.print(",");
    
    // FirstSeen (timestamp) - use GPS time if available, else millis
    GPSData gps = GPS::getData();
    if (gps.date > 0 && gps.time > 0) {
        // date format: DDMMYY, time format: HHMMSSCC
        uint8_t day = gps.date / 10000;
        uint8_t month = (gps.date / 100) % 100;
        uint8_t year = gps.date % 100;
        uint8_t hour = gps.time / 1000000;
        uint8_t minute = (gps.time / 10000) % 100;
        uint8_t second = (gps.time / 100) % 100;
        f.printf("20%02d-%02d-%02d %02d:%02d:%02d,", year, month, day, hour, minute, second);
    } else {
        // Fallback - use boot time reference
        f.printf("1970-01-01 00:00:%02d,", (millis() / 1000) % 60);
    }
    
    // Channel
    f.printf("%d,", channel);
    
    // Frequency (calculate from channel for 2.4GHz)
    int freq = 2412 + (channel - 1) * 5;
    if (channel == 14) freq = 2484;  // Special case for channel 14
    f.printf("%d,", freq);
    
    // RSSI
    f.printf("%d,", rssi);
    
    // Latitude, Longitude, Altitude
    f.printf("%.6f,%.6f,%.1f,", lat, lon, alt);
    
    // AccuracyMeters (GPS HDOP as accuracy estimate, or default 10m)
    f.printf("%.1f,", accuracy > 0 ? accuracy : 10.0);
    
    // RCOIs (empty), MfgrId (empty), Type (WIFI)
    f.println(",,WIFI");
    
    f.close();
}

void WarhogMode::processScanResults() {
    int n = scanResult;
    
    if (n < 0) {
        Serial.println("[WARHOG] No valid scan results");
        WiFi.scanDelete();
        return;
    }
    
    // Get current GPS data - check for valid fix
    GPSData gpsData = GPS::getData();
    bool hasGPS = GPS::hasFix() && (gpsData.latitude != 0.0 && gpsData.longitude != 0.0);
    
    SDLOG("WARHOG", "Processing %d networks (GPS: %s)", n, hasGPS ? "yes" : "no");
    
    // Guard beacon map access from promiscuous callback
    beaconMapBusy = true;
    
    uint32_t newThisScan = 0;
    uint32_t geotaggedThisScan = 0;
    
    for (int i = 0; i < n; i++) {
        uint8_t* bssidPtr = WiFi.BSSID(i);
        if (!bssidPtr) continue;
        
        uint64_t bssidKey = bssidToKey(bssidPtr);
        
        // Skip if already processed this session
        if (seenBSSIDs.count(bssidKey) > 0) {
            continue;
        }
        
        // Add to seen set immediately (before any file writes)
        if (seenBSSIDs.size() < MAX_SEEN_BSSIDS) {
            seenBSSIDs.insert(bssidKey);
        }
        
        // Extract network info
        String ssidStr = WiFi.SSID(i);
        const char* ssid = ssidStr.c_str();
        int8_t rssi = WiFi.RSSI(i);
        uint8_t channel = WiFi.channel(i);
        wifi_auth_mode_t authmode = WiFi.encryptionType(i);
        
        // Extract ML features
        WiFiFeatures features;
        if (enhancedMode) {
            auto it = beaconFeatures.find(bssidKey);
            if (it != beaconFeatures.end()) {
                features = it->second;
                features.rssi = rssi;
                features.snr = (float)(rssi - features.noise);
            } else {
                features = FeatureExtractor::extractBasic(rssi, channel, authmode);
            }
        } else {
            features = FeatureExtractor::extractBasic(rssi, channel, authmode);
        }
        
        // Update statistics
        totalNetworks++;
        newThisScan++;
        
        // Track auth types
        switch (authmode) {
            case WIFI_AUTH_OPEN:
                openNetworks++;
                XP::addXP(XPEvent::NETWORK_OPEN);
                break;
            case WIFI_AUTH_WEP:
                wepNetworks++;
                XP::addXP(XPEvent::NETWORK_WEP);
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
        
        // Write to files based on GPS status
        if (Config::isSDAvailable()) {
            if (hasGPS) {
                // Full wardriving: both CSV, WiGLE, and ML
                appendCSVEntry(bssidPtr, ssid, rssi, channel, authmode,
                              gpsData.latitude, gpsData.longitude, gpsData.altitude);
                
                // WiGLE format export (HDOP * 5 as rough accuracy estimate in meters)
                double accuracy = gpsData.hdop > 0 ? gpsData.hdop * 5.0 : 10.0;
                appendWigleEntry(bssidPtr, ssid, rssi, channel, authmode,
                                gpsData.latitude, gpsData.longitude, gpsData.altitude, accuracy);
                
                savedCount++;
                geotaggedThisScan++;
                XP::addXP(XPEvent::WARHOG_LOGGED);  // +2 XP for geotagged network
                
                if (enhancedMode) {
                    appendMLEntry(bssidPtr, ssid, features, 0,
                                 gpsData.latitude, gpsData.longitude);
                }
            } else {
                // No GPS: ML only (if Enhanced mode)
                if (enhancedMode) {
                    appendMLEntry(bssidPtr, ssid, features, 0, 0, 0);
                    mlOnlyCount++;
                }
            }
        }
        
        Serial.printf("[WARHOG] New: %s (ch%d, %s)%s\n",
                     ssid, channel, authModeToString(authmode).c_str(),
                     hasGPS ? " [GPS]" : "");
    }
    
    // Release beacon map guard
    beaconMapBusy = false;
    
    // Trigger mood update if we found new networks
    if (newThisScan > 0) {
        Mood::onWarhogFound(nullptr, 0);
        SDLOG("WARHOG", "Found %lu new (%lu geotagged)", newThisScan, geotaggedThisScan);
    }
    
    WiFi.scanDelete();
    
    // Re-enable promiscuous mode for beacon capture if Enhanced mode
    if (enhancedMode) {
        esp_wifi_set_promiscuous(true);
    }
}

bool WarhogMode::hasGPSFix() {
    return GPS::hasFix();
}

GPSData WarhogMode::getGPSData() {
    return GPS::getData();
}

// Export functions - data is already on disk, these are for format conversion
// They now read from the session CSV and convert format

bool WarhogMode::exportCSV(const char* path) {
    // Data is already in currentFilename as CSV
    // This function would copy/rename, but for now just log
    Serial.printf("[WARHOG] CSV data already in: %s\n", currentFilename.c_str());
    return currentFilename.length() > 0;
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

bool WarhogMode::exportMLTraining(const char* path) {
    // ML data is already on disk in currentMLFilename
    Serial.printf("[WARHOG] ML data already in: %s\n", currentMLFilename.c_str());
    return currentMLFilename.length() > 0;
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
        // No GPS time - use millis with random suffix for uniqueness
        snprintf(buf, sizeof(buf), "/wardriving/warhog_%lu_%04X.%s", 
                millis(), (uint16_t)esp_random(), ext);
    }
    
    return String(buf);
}

// Enhanced ML Mode - Promiscuous beacon capture

void WarhogMode::promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    if (beaconMapBusy) return;
    
    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* frame = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;
    int8_t rssi = pkt->rx_ctrl.rssi;
    
    if (len < 24) return;
    
    uint16_t frameControl = frame[0] | (frame[1] << 8);
    uint8_t frameType = (frameControl >> 2) & 0x03;
    uint8_t frameSubtype = (frameControl >> 4) & 0x0F;
    
    if (frameType != 0) return;
    if (frameSubtype != 8 && frameSubtype != 5) return;
    
    const uint8_t* bssid = frame + 16;
    uint64_t key = bssidToKey(bssid);
    
    WiFiFeatures features = FeatureExtractor::extractFromBeacon(frame, len, rssi);
    
    if (beaconFeatures.size() < 500) {
        auto it = beaconFeatures.find(key);
        if (it != beaconFeatures.end()) {
            it->second.beaconCount++;
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
