// Warhog Mode implementation - Refactored "GPS as Gate" architecture
// 
// Key changes from original:
// - No entries[] vector - data goes directly to disk
// - No "waiting for GPS" state - either GPS or ML-only
// - Simpler memory management - Bloom filter for duplicate detection
// - Per-network file writes instead of batch saves

#include "warhog.h"
#include "oink.h"
#include "../build_info.h"
#include "../core/config.h"
#include "../core/wifi_utils.h"
#include "../core/heap_policy.h"
#include "../core/heap_health.h"
#include "../core/network_recon.h"
#include "../core/janus_hog.h"
#include "../core/wsl_bypasser.h"
#include "../core/sdlog.h"
#include "../core/sd_layout.h"
#include "../core/xp.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>
#include <esp_heap_caps.h>

// Bloom filter for seen BSSIDs (fixed memory, no heap churn)
// 4KB = 32,768 bits -> ~1.5% false positives around 5k entries with 3 hashes
static const size_t SEEN_BLOOM_BYTES = 4096;
static const size_t SEEN_BLOOM_BITS = SEEN_BLOOM_BYTES * 8;
static const size_t SEEN_BLOOM_MASK = SEEN_BLOOM_BITS - 1;
static const uint8_t SEEN_BLOOM_HASHES = 3;
static_assert((SEEN_BLOOM_BITS & (SEEN_BLOOM_BITS - 1)) == 0, "SEEN_BLOOM_BITS must be power of two");

// Captured bloom for bounty exclusion (small, fast)
static const size_t CAPTURED_BLOOM_BYTES = 2048;
static const size_t CAPTURED_BLOOM_BITS = CAPTURED_BLOOM_BYTES * 8;
static const size_t CAPTURED_BLOOM_MASK = CAPTURED_BLOOM_BITS - 1;
static const uint8_t CAPTURED_BLOOM_HASHES = 3;
static_assert((CAPTURED_BLOOM_BITS & (CAPTURED_BLOOM_BITS - 1)) == 0, "CAPTURED_BLOOM_BITS must be power of two");

// Bounty pool (reservoir sample of seen networks)
static const size_t BOUNTY_POOL_SIZE = 50;

// Heap threshold for emergency cleanup (bytes) - centralized in HeapPolicy

// Minimum scan interval to avoid tight-loop scanning
static const uint32_t SCAN_INTERVAL_MIN_MS = 1000;

// SD card retry settings (SD can be busy with other operations)
static const int SD_RETRY_COUNT = 3;
static const int SD_RETRY_DELAY_MS = 10;

// WiGLE file size limit for upload compatibility (400KB - leave room for headers)
// Files larger than this will be rotated to a new file
static const size_t WIGLE_FILE_MAX_SIZE = 400000;

// Graceful stop request flag for background scan task (atomic: cross-core visibility)
static std::atomic<bool> stopRequested{false};
// Set by scan task just before self-deleting, used for safe cleanup in stop()
static std::atomic<bool> scanTaskExited{false};

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
static uint8_t seenBloom[SEEN_BLOOM_BYTES];
static uint8_t capturedBloom[CAPTURED_BLOOM_BYTES];
static uint64_t bountyPool[BOUNTY_POOL_SIZE];
static uint16_t bountyPoolCount = 0;
static uint32_t bountySeenTotal = 0;
uint32_t WarhogMode::totalNetworks = 0;
uint32_t WarhogMode::openNetworks = 0;
uint32_t WarhogMode::wepNetworks = 0;
uint32_t WarhogMode::wpaNetworks = 0;
uint32_t WarhogMode::savedCount = 0;      // Geotagged networks (CSV)
char WarhogMode::currentFilename[128] = {0};
char WarhogMode::currentWigleFilename[128] = {0};

// Scan state
volatile bool WarhogMode::scanInProgress = false;
uint32_t WarhogMode::scanStartTime = 0;

// Background scan task statics
TaskHandle_t WarhogMode::scanTaskHandle = NULL;
std::atomic<int> WarhogMode::scanResult{-2};  // -2 = not started, -1 = running, >=0 = complete

// Scan task check: returns true if should abort
static inline bool shouldAbortScan() {
    return stopRequested || !WarhogMode::isRunning();
}

static uint32_t mix32(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

static bool bloomTest(const uint8_t* bloom, size_t mask, uint8_t hashes, uint64_t key) {
    uint32_t h1 = mix32(key);
    uint32_t h2 = mix32(key ^ 0x9e3779b97f4a7c15ULL) | 1U;
    for (uint8_t i = 0; i < hashes; i++) {
        uint32_t idx = (h1 + (uint32_t)i * h2) & (uint32_t)mask;
        if ((bloom[idx >> 3] & (1 << (idx & 7))) == 0) {
            return false;
        }
    }
    return true;
}

static void bloomAdd(uint8_t* bloom, size_t mask, uint8_t hashes, uint64_t key) {
    uint32_t h1 = mix32(key);
    uint32_t h2 = mix32(key ^ 0x9e3779b97f4a7c15ULL) | 1U;
    for (uint8_t i = 0; i < hashes; i++) {
        uint32_t idx = (h1 + (uint32_t)i * h2) & (uint32_t)mask;
        bloom[idx >> 3] |= (1 << (idx & 7));
    }
}

static void resetSeenTracking() {
    memset(seenBloom, 0, sizeof(seenBloom));
    memset(capturedBloom, 0, sizeof(capturedBloom));
    bountyPoolCount = 0;
    bountySeenTotal = 0;
}

static void seedCapturedFromOink() {
    for (const auto& hs : OinkMode::getHandshakes()) {
        bloomAdd(capturedBloom, CAPTURED_BLOOM_MASK, CAPTURED_BLOOM_HASHES, bssidToKey(hs.bssid));
    }
    for (const auto& p : OinkMode::getPMKIDs()) {
        bloomAdd(capturedBloom, CAPTURED_BLOOM_MASK, CAPTURED_BLOOM_HASHES, bssidToKey(p.bssid));
    }
}

static uint32_t clampScanIntervalMs(uint32_t intervalMs) {
    return (intervalMs < SCAN_INTERVAL_MIN_MS) ? SCAN_INTERVAL_MIN_MS : intervalMs;
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
    totalNetworks = 0;
    openNetworks = 0;
    wepNetworks = 0;
    wpaNetworks = 0;
    savedCount = 0;
    currentFilename[0] = '\0';
    currentWigleFilename[0] = '\0';

    resetSeenTracking();

    scanInterval = clampScanIntervalMs(Config::gps().updateInterval * 1000UL);
}

void WarhogMode::start() {
    if (running) return;

    // Clear previous session data
    totalNetworks = 0;
    openNetworks = 0;
    wepNetworks = 0;
    wpaNetworks = 0;
    savedCount = 0;
    currentFilename[0] = '\0';
    currentWigleFilename[0] = '\0';

    resetSeenTracking();
    seedCapturedFromOink();

    // Reset distance tracking for XP
    lastGPSLat = 0;
    lastGPSLon = 0;
    lastDistanceCheck = 0;
    
    // Reload scan interval from config
    scanInterval = clampScanIntervalMs(Config::gps().updateInterval * 1000UL);
    
    // Reset stop flag for clean start
    stopRequested = false;

    // Stop NetworkRecon before WiFi manipulation (uses promiscuous mode, incompatible with STA scanning)
    NetworkRecon::stop();
    
    // Soft WiFi reset — keep driver alive to avoid esp_wifi_init() RX buffer failures
    WiFi.disconnect(false, true);  // Keep driver, erase AP credentials
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

    // Ensure GPS is in continuous mode — unless C5 is using the same pins
    // (GPS was intentionally slept by JanusHog::init() to avoid UART conflict)
    if (GPS::isActive() || !JanusHog::isConnected()) {
        GPS::ensureContinuousMode();
    }
    
    running = true;
    lastScanTime = 0;  // Trigger immediate scan
    
    // Set grass speed for wardriving - animation controlled by GPS lock in update()
    Avatar::setGrassSpeed(200);  // Slower than OINK (~5 FPS)
    Avatar::setGrassMoving(GPS::hasFix() || JanusHog::hasC5GPSFix());
    
    Display::setWiFiStatus(true);
    Mood::onWarhogUpdate();  // Show WARHOG phrase on start
    Mood::setDialogueLock(true);
}


void WarhogMode::stop() {
    if (!running) return;
    
    // Signal task to stop gracefully
    stopRequested = true;

    // Wait for background scan task to exit
    if (scanInProgress && scanTaskHandle != NULL) {
        // Wait for scanTaskExited (the authoritative atomic flag) — not the handle,
        // which is racy between the task self-deleting and us reading it.
        for (int i = 0; i < 10 && !scanTaskExited.load(); i++) {
            delay(50);
        }
        if (scanTaskExited.load()) {
            // Task exited cleanly — it already called vTaskDelete(NULL).
            // Handle may or may not be NULL yet; just clear it.
            scanTaskHandle = NULL;
            WiFi.scanDelete();
        } else {
            // Task didn't exit in time (stuck in WiFi.scanNetworks blocking call).
            // Force-delete with the saved handle.
            Serial.println("[WARHOG] Force-deleting scan task");
            TaskHandle_t h = scanTaskHandle;
            scanTaskHandle = NULL;
            if (h) vTaskDelete(h);
            // WiFi state may be inconsistent after force-kill.
            // Soft reset keeps driver alive (avoid RX buffer realloc on fragmented heap).
            WiFi.disconnect(false, true);
            delay(50);
        }
    }
    scanInProgress = false;
    scanResult = -2;
    
    // Stop grass animation
    Avatar::setGrassMoving(false);
    Avatar::waveRipple(WaveMode::NONE);

    running = false;
    
    // Put GPS to sleep if power management enabled
    if (Config::gps().powerSave) {
        GPS::sleep();
    }
    
    // Restart NetworkRecon (restores promiscuous mode for OINK/DNH/etc)
    NetworkRecon::start();
    Display::setWiFiStatus(true);  // Recon is active
    
    // Reset stop flag for next run
    stopRequested = false;
    Mood::setDialogueLock(false);
}

// Background task for WiFi scanning - runs sync scan without blocking main loop
void WarhogMode::scanTask(void* pvParameters) {
    // Check for early abort request
    if (shouldAbortScan()) {
        scanResult = -2;
        scanTaskExited = true;
        scanTaskHandle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Soft WiFi reset — keep driver alive to avoid RX buffer realloc failures
    WiFi.scanDelete();
    WiFi.disconnect(false, true);
        vTaskDelay(pdMS_TO_TICKS(100));

    // Check abort between WiFi operations
    if (shouldAbortScan()) {
        scanResult = -2;
        scanTaskExited = true;
        scanTaskHandle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    WiFi.mode(WIFI_STA);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Sync scan - this blocks until complete (which is fine in background task)
    int result = WiFi.scanNetworks(false, true);  // sync, show hidden
    
    // Store result for main loop to pick up
    scanResult = result;

    // Log stack usage for sizing decisions
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    Serial.printf("[WARHOG] Scan task stack HWM: %u bytes unused of 4096\n",
                  (unsigned)(hwm * sizeof(StackType_t)));

    // Signal clean exit, then self-delete
    scanTaskExited = true;
    scanTaskHandle = NULL;
    vTaskDelete(NULL);
}

void WarhogMode::update() {
    if (!running) return;
    
    uint32_t now = millis();
    static uint32_t lastPhraseTime = 0;
    static bool lastGPSState = false;
    static uint32_t lastHeapCheck = 0;
    static uint32_t lastC5ScanReqMs = 0;
    
    // Periodic heap monitoring (every 30 seconds)
    if (now - lastHeapCheck >= 30000) {
        if (HeapHealth::getPressureLevel() >= HeapPressureLevel::Critical) {
            Display::showToast("LOW MEMORY!");
        }
        lastHeapCheck = now;
    }

    // Update grass animation based on GPS fix status (local or C5)
    bool hasGPSFix = GPS::hasFix() || JanusHog::hasC5GPSFix();
    if (hasGPSFix != lastGPSState) {
        Avatar::setGrassMoving(hasGPSFix);
        lastGPSState = hasGPSFix;
    }

    // Continuous sat-stream waves: pulse INCOMING while GPS active
    static uint32_t lastWaveRefresh = 0;
    if (hasGPSFix && now - lastWaveRefresh >= 2400) {  // match CYCLE_MS
        GPSData gps = GPS::hasFix() ? GPS::getData() : JanusHog::getC5GPSData();
        uint8_t sats = gps.satellites;
        // Map sat count to wave intensity (ring count 1-3)
        uint8_t intensity = (sats >= 8) ? 3 : (sats >= 5) ? 2 : 1;
        Avatar::waveRipple(WaveMode::INCOMING, intensity);
        lastWaveRefresh = now;
    }

    // Distance tracking for XP (every 5 seconds when GPS is available)
    if (hasGPSFix && now - lastDistanceCheck >= 5000) {
        GPSData gps = GPS::hasFix() ? GPS::getData() : JanusHog::getC5GPSData();
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
            scanInProgress = false;
            processScanResults();
            scanResult = -2;  // Reset for next scan
        } else if (scanTaskHandle == NULL && scanResult == -2) {
            // Task ended but no result - something went wrong
            scanInProgress = false;
        } else if (now - scanStartTime > 20000) {
            // Timeout after 20 seconds
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
        // Best-effort: keep C5 scan data fresh for dual-band logging.
        if (JanusHog::isConnected() && JanusHog::isReady()) {
            if (lastC5ScanReqMs == 0 || (now - lastC5ScanReqMs) >= 30000) {
                (void)JanusHog::requestScan();
                lastC5ScanReqMs = now;
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

    scanInProgress = true;
    scanStartTime = millis();
    scanResult = -1;  // Running
    scanTaskExited = false;

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
        // Fallback: run sync scan on main thread if task creation fails
        scanInProgress = false;
        scanResult = WiFi.scanNetworks(false, true);
        if (scanResult >= 0) {
            processScanResults();
        }
        scanResult = -2;
    }
}

// Ensure CSV file exists with header
bool WarhogMode::ensureCSVFileReady() {
    if (currentFilename[0] != '\0') return true;

    // Ensure wardriving directory exists
    const char* wardrivingDir = SDLayout::wardrivingDir();
    if (!SD.exists(wardrivingDir)) {
        if (!SD.mkdir(wardrivingDir)) {
            return false;
        }
    }

    generateFilename(currentFilename, sizeof(currentFilename), "csv");

    File f = openFileWithRetry(currentFilename, FILE_WRITE);
    if (!f) {
        currentFilename[0] = '\0';
        return false;
    }
    
    f.println("BSSID,SSID,RSSI,Channel,AuthMode,Latitude,Longitude,Altitude,Timestamp");
    f.close();
    
    return true;
}

// Append single network to CSV file
void WarhogMode::appendCSVEntry(const uint8_t* bssid, const char* ssid,
                                 int8_t rssi, uint8_t channel, wifi_auth_mode_t auth,
                                 double lat, double lon, double alt) {
    if (!ensureCSVFileReady()) return;
    
    File f = openFileWithRetry(currentFilename, FILE_APPEND);
    if (!f) return;

    f.printf("%02X:%02X:%02X:%02X:%02X:%02X,",
            bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    writeCSVField(f, ssid);
    f.print(",");
    f.printf("%d,%d,%s,%.6f,%.6f,%.1f,%lu\n",
            rssi, channel, authModeToString(auth),
            lat, lon, alt, millis());
    f.close();
}

// Check if WiGLE file needs rotation due to size
void WarhogMode::checkWigleFileRotation() {
    if (currentWigleFilename[0] == '\0') return;

    File f = SD.open(currentWigleFilename, FILE_READ);
    if (!f) return;

    size_t fileSize = f.size();
    f.close();

    if (fileSize >= WIGLE_FILE_MAX_SIZE) {
        currentWigleFilename[0] = '\0';  // Force new file creation on next append
    }
}

// Ensure WiGLE file exists with header
bool WarhogMode::ensureWigleFileReady() {
    // Check if current file needs rotation
    checkWigleFileRotation();
    
    if (currentWigleFilename[0] != '\0') return true;

    // Ensure wardriving directory exists
    const char* wardrivingDir = SDLayout::wardrivingDir();
    if (!SD.exists(wardrivingDir)) {
        if (!SD.mkdir(wardrivingDir)) {
            return false;
        }
    }

    generateFilename(currentWigleFilename, sizeof(currentWigleFilename), "wigle.csv");

    File f = openFileWithRetry(currentWigleFilename, FILE_WRITE);
    if (!f) {
        currentWigleFilename[0] = '\0';
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
    
    return true;
}

// Convert auth mode to WiGLE capability string format (returns string literal, zero allocation)
const char* WarhogMode::authModeToWigleString(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN:          return "[ESS]";
        case WIFI_AUTH_WEP:           return "[WEP][ESS]";
        case WIFI_AUTH_WPA_PSK:       return "[WPA-PSK-CCMP][ESS]";
        case WIFI_AUTH_WPA2_PSK:      return "[WPA2-PSK-CCMP][ESS]";
        case WIFI_AUTH_WPA_WPA2_PSK:  return "[WPA-PSK-CCMP+TKIP][WPA2-PSK-CCMP+TKIP][ESS]";
        case WIFI_AUTH_WPA3_PSK:      return "[WPA3-SAE][ESS]";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "[WPA2-PSK-CCMP][WPA3-SAE][ESS]";
        case WIFI_AUTH_WAPI_PSK:      return "[WAPI-PSK][ESS]";
        default:                      return "[ESS]";
    }
}

static int channelToFrequency(uint8_t channel) {
    if (channel >= 1 && channel <= 13) {
        return 2412 + (channel - 1) * 5;
    }
    if (channel == 14) {
        return 2484;
    }
    if (channel <= 196) {
        return 5000 + channel * 5;
    }
    if (channel <= 233) {
        return 5950 + channel * 5;
    }
    return 0;
}

// Append single network to WiGLE file
void WarhogMode::appendWigleEntry(const uint8_t* bssid, const char* ssid,
                                   int8_t rssi, uint8_t channel, wifi_auth_mode_t auth,
                                   double lat, double lon, double alt, double accuracy) {
    if (!ensureWigleFileReady()) return;
    
    File f = openFileWithRetry(currentWigleFilename, FILE_APPEND);
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
    GPSData gps = GPS::hasFix() ? GPS::getData() : JanusHog::getC5GPSData();
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
    
    // Frequency (best-effort mapping for 2.4/5/6 GHz)
    int freq = channelToFrequency(channel);
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
        WiFi.scanDelete();
        return;
    }
    
    // Get current GPS data - check for valid fix, fall back to C5 GPS if local unavailable
    GPSData gpsData = GPS::getData();
    bool hasGPS = GPS::hasFix();
    if (!hasGPS && JanusHog::hasC5GPSFix()) {
        gpsData = JanusHog::getC5GPSData();
        hasGPS = true;
    }
    
    SDLOG("WARHOG", "Processing %d networks (GPS: %s)", n, hasGPS ? "yes" : "no");
    
    uint32_t newThisScan = 0;
    uint32_t geotaggedThisScan = 0;
    
    // Process each network with periodic yield to prevent WDT issues
    for (int i = 0; i < n; i++) {
        // Yield periodically during long processing loops to prevent WDT
        if (i % 5 == 0) {
            yield(); // Allow other tasks to run
        }

        uint8_t* bssidPtr = WiFi.BSSID(i);
        if (!bssidPtr) continue;
        
        uint64_t bssidKey = bssidToKey(bssidPtr);
        
        // Skip if already processed this session (Bloom filter)
        if (bloomTest(seenBloom, SEEN_BLOOM_MASK, SEEN_BLOOM_HASHES, bssidKey)) {
            continue;
        }

        // Mark as seen and update bounty reservoir before any file writes
        bloomAdd(seenBloom, SEEN_BLOOM_MASK, SEEN_BLOOM_HASHES, bssidKey);
        if (bountySeenTotal < UINT32_MAX) bountySeenTotal++;
        if (bountyPoolCount < BOUNTY_POOL_SIZE) {
            bountyPool[bountyPoolCount++] = bssidKey;
        } else {
            uint32_t pick = esp_random() % bountySeenTotal;
            if (pick < BOUNTY_POOL_SIZE) {
                bountyPool[pick] = bssidKey;
            }
        }
        
        // Extract SSID to stack buffer — avoids heap String for each of 50+ networks
        char ssidBuf[33];
        strncpy(ssidBuf, WiFi.SSID(i).c_str(), sizeof(ssidBuf) - 1);
        ssidBuf[sizeof(ssidBuf) - 1] = '\0';
        const char* ssid = ssidBuf;
        int8_t rssi = WiFi.RSSI(i);
        uint8_t channel = WiFi.channel(i);
        wifi_auth_mode_t authmode = WiFi.encryptionType(i);
        
        // Validate extracted data to prevent potential crashes
        if (!ssid || strlen(ssid) > 32) continue;
        if (channel == 0 || channel > 165) continue; // Valid WiFi channels are 1-165

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
                if (gpsData.coasting && accuracy < 30.0) accuracy = 30.0;
                appendWigleEntry(bssidPtr, ssid, rssi, channel, authmode,
                                gpsData.latitude, gpsData.longitude, gpsData.altitude, accuracy);
                
                savedCount++;
                geotaggedThisScan++;
                XP::addXP(XPEvent::WARHOG_LOGGED);  // +2 XP for geotagged network
            }
        }
    }

    // --- Dual-band add-on: 5GHz networks from JanusHog (injected into recon) ---
    if (JanusHog::isConnected()) {
        struct C5Net {
            uint8_t bssid[6];
            char ssid[33];
            int8_t rssi;
            uint8_t channel;
            wifi_auth_mode_t authmode;
        };

        static C5Net c5nets[64]; // bounded snapshot, avoids holding recon lock during SD writes
        uint8_t c5count = 0;

        uint32_t nowMs = millis();
        NetworkRecon::enterCritical();
        for (const auto& net : NetworkRecon::getNetworks()) {
            if (net.source != NET_SOURCE_C5) continue;
            if (net.channel <= 14 || net.channel > 165) continue;
            // Keep wardrive output "live" even if UI keeps C5 nets longer.
            if (nowMs - net.lastSeen > 120000) continue;
            if (c5count >= (uint8_t)(sizeof(c5nets) / sizeof(c5nets[0]))) break;

            memcpy(c5nets[c5count].bssid, net.bssid, 6);
            strncpy(c5nets[c5count].ssid, net.ssid, 32);
            c5nets[c5count].ssid[32] = '\0';
            c5nets[c5count].rssi = (net.rssiAvg != 0) ? net.rssiAvg : net.rssi;
            c5nets[c5count].channel = net.channel;
            c5nets[c5count].authmode = net.authmode;
            c5count++;
        }
        NetworkRecon::exitCritical();

        for (uint8_t i = 0; i < c5count; i++) {
            const C5Net& cn = c5nets[i];
            uint64_t bssidKey = bssidToKey(cn.bssid);

            if (bloomTest(seenBloom, SEEN_BLOOM_MASK, SEEN_BLOOM_HASHES, bssidKey)) {
                continue;
            }

            bloomAdd(seenBloom, SEEN_BLOOM_MASK, SEEN_BLOOM_HASHES, bssidKey);
            if (bountySeenTotal < UINT32_MAX) bountySeenTotal++;
            if (bountyPoolCount < BOUNTY_POOL_SIZE) {
                bountyPool[bountyPoolCount++] = bssidKey;
            } else {
                uint32_t pick = esp_random() % bountySeenTotal;
                if (pick < BOUNTY_POOL_SIZE) {
                    bountyPool[pick] = bssidKey;
                }
            }

            // Basic validation (mirror local scan guards)
            if (cn.channel == 0 || cn.channel > 165) continue;

            totalNetworks++;
            newThisScan++;

            switch (cn.authmode) {
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

            if (Config::isSDAvailable() && hasGPS) {
                appendCSVEntry(cn.bssid, cn.ssid, cn.rssi, cn.channel, cn.authmode,
                               gpsData.latitude, gpsData.longitude, gpsData.altitude);

                double accuracy = gpsData.hdop > 0 ? gpsData.hdop * 5.0 : 10.0;
                if (gpsData.coasting && accuracy < 30.0) accuracy = 30.0;
                appendWigleEntry(cn.bssid, cn.ssid, cn.rssi, cn.channel, cn.authmode,
                                 gpsData.latitude, gpsData.longitude, gpsData.altitude, accuracy);

                savedCount++;
                geotaggedThisScan++;
                XP::addXP(XPEvent::WARHOG_LOGGED);
            }
        }
    }
    
    // Trigger mood update if we found new networks
    if (newThisScan > 0) {
        Mood::onWarhogFound(nullptr, 0);
        SDLOG("WARHOG", "Found %lu new (%lu geotagged)", newThisScan, geotaggedThisScan);
    }
    
    WiFi.scanDelete();
}

bool WarhogMode::hasGPSFix() {
    return GPS::hasFix() || JanusHog::hasC5GPSFix();
}

GPSData WarhogMode::getGPSData() {
    return GPS::hasFix() ? GPS::getData() : JanusHog::getC5GPSData();
}

// Export functions - data is already on disk, these are for format conversion
// They now read from the session CSV and convert format

bool WarhogMode::exportCSV(const char* path) {
    // Data is already in currentFilename as CSV
    // This function would copy/rename, but for now just return status
    return currentFilename[0] != '\0';
}


const char* WarhogMode::authModeToString(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN:          return "OPEN";
        case WIFI_AUTH_WEP:           return "WEP";
        case WIFI_AUTH_WPA_PSK:       return "WPA";
        case WIFI_AUTH_WPA2_PSK:      return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK:      return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        case WIFI_AUTH_WAPI_PSK:      return "WAPI";
        default:                      return "UNKNOWN";
    }
}

// === BOUNTY SYSTEM (Phase 5) ===
// Track which BSSIDs were actually captured (handshakes/PMKIDs) so Papa only sends misses
void WarhogMode::markCaptured(const uint8_t* bssid) {
    if (!bssid) return;
    
    uint64_t key = bssidToKey(bssid);
    bloomAdd(capturedBloom, CAPTURED_BLOOM_MASK, CAPTURED_BLOOM_HASHES, key);

    // Remove from bounty pool if present
    for (uint16_t i = 0; i < bountyPoolCount; i++) {
        if (bountyPool[i] == key) {
            bountyPool[i] = bountyPool[bountyPoolCount - 1];
            bountyPoolCount--;
            break;
        }
    }
}

std::vector<uint64_t> WarhogMode::getUnclaimedBSSIDs() {
    std::vector<uint64_t> unclaimed;
    unclaimed.reserve(bountyPoolCount);
    for (uint16_t i = 0; i < bountyPoolCount; i++) {
        uint64_t key = bountyPool[i];
        if (bloomTest(capturedBloom, CAPTURED_BLOOM_MASK, CAPTURED_BLOOM_HASHES, key)) {
            continue;
        }
        unclaimed.push_back(key);
    }
    return unclaimed;
}

void WarhogMode::buildBountyList(uint8_t* buffer, uint8_t* count) {
    if (!buffer || !count) return;
    
    auto unclaimed = getUnclaimedBSSIDs();
    *count = 0;
    
    for (uint64_t key : unclaimed) {
        if (*count >= MAX_BOUNTIES) break;  // Max 15 bounties
        buffer[*count * 6 + 0] = (key >> 40) & 0xFF;
        buffer[*count * 6 + 1] = (key >> 32) & 0xFF;
        buffer[*count * 6 + 2] = (key >> 24) & 0xFF;
        buffer[*count * 6 + 3] = (key >> 16) & 0xFF;
        buffer[*count * 6 + 4] = (key >> 8) & 0xFF;
        buffer[*count * 6 + 5] = key & 0xFF;
        (*count)++;
    }
}

void WarhogMode::generateFilename(char* buf, size_t bufSize, const char* ext) {
    GPSData gps = GPS::getData();
    const char* wardrivingDir = SDLayout::wardrivingDir();

    if (gps.date > 0 && gps.time > 0) {
        uint8_t day = gps.date / 10000;
        uint8_t month = (gps.date / 100) % 100;
        uint8_t year = gps.date % 100;
        uint8_t hour = gps.time / 1000000;
        uint8_t minute = (gps.time / 10000) % 100;
        uint8_t second = (gps.time / 100) % 100;

        snprintf(buf, bufSize, "%s/warhog_20%02d%02d%02d_%02d%02d%02d.%s",
                wardrivingDir,
                year, month, day, hour, minute, second, ext);
    } else {
        snprintf(buf, bufSize, "%s/warhog_%lu_%04X.%s",
                wardrivingDir,
                millis(), (uint16_t)esp_random(), ext);
    }
}
