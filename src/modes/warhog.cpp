// Warhog Mode implementation

#include "warhog.h"
#include "../core/config.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include "../ml/features.h"
#include "../ml/inference.h"
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>

// Maximum entries to prevent memory exhaustion (~120 bytes each, 2000 = ~240KB)
static const size_t MAX_ENTRIES = 2000;

// Static members
bool WarhogMode::running = false;
uint32_t WarhogMode::lastScanTime = 0;
uint32_t WarhogMode::scanInterval = 5000;
std::vector<WardrivingEntry> WarhogMode::entries;
size_t WarhogMode::newCount = 0;
uint32_t WarhogMode::totalNetworks = 0;
uint32_t WarhogMode::openNetworks = 0;
uint32_t WarhogMode::wepNetworks = 0;
uint32_t WarhogMode::wpaNetworks = 0;
uint32_t WarhogMode::savedCount = 0;
String WarhogMode::currentFilename = "";

void WarhogMode::init() {
    entries.clear();
    newCount = 0;
    totalNetworks = 0;
    openNetworks = 0;
    wepNetworks = 0;
    wpaNetworks = 0;
    savedCount = 0;
    currentFilename = "";
    
    scanInterval = Config::gps().updateInterval * 1000;
    
    Serial.println("[WARHOG] Initialized");
}

void WarhogMode::start() {
    if (running) return;
    
    Serial.println("[WARHOG] Starting...");
    
    // Clear previous session data
    entries.clear();
    totalNetworks = 0;
    openNetworks = 0;
    wepNetworks = 0;
    wpaNetworks = 0;
    savedCount = 0;
    currentFilename = "";
    
    // Initialize WiFi in STA mode for scanning
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    
    // Wake up GPS
    GPS::wake();
    
    running = true;
    lastScanTime = 0;  // Trigger immediate scan
    newCount = 0;
    
    Display::setWiFiStatus(true);
    Mood::onWarhogUpdate();  // Show WARHOG phrase on start
    Serial.println("[WARHOG] Running");
}

void WarhogMode::stop() {
    if (!running) return;
    
    Serial.println("[WARHOG] Stopping...");
    
    running = false;
    
    // Put GPS to sleep if power management enabled
    if (Config::gps().powerSave) {
        GPS::sleep();
    }
    
    Display::setWiFiStatus(false);
    
    Serial.println("[WARHOG] Stopped");
}

void WarhogMode::update() {
    if (!running) return;
    
    uint32_t now = millis();
    static uint32_t lastPhraseTime = 0;
    
    // Rotate phrases every 5 seconds when idle
    if (now - lastPhraseTime >= 5000) {
        Mood::onWarhogUpdate();
        lastPhraseTime = now;
    }
    
    // Check if async scan completed
    int scanResult = WiFi.scanComplete();
    if (scanResult >= 0) {
        // Scan done with results
        processScanResults();
    } else if (scanResult == WIFI_SCAN_FAILED) {
        // Scan failed, reset
        WiFi.scanDelete();
    }
    
    // Periodic scanning - only start new scan if not already scanning
    if (now - lastScanTime >= scanInterval && scanResult != WIFI_SCAN_RUNNING) {
        performScan();
        lastScanTime = now;
    }
}

void WarhogMode::triggerScan() {
    performScan();
}

bool WarhogMode::isScanComplete() {
    return WiFi.scanComplete() >= 0;
}

void WarhogMode::performScan() {
    Serial.println("[WARHOG] Starting WiFi scan...");
    
    // Start async scan
    int result = WiFi.scanNetworks(true, true);  // async=true, hidden=true
    
    if (result == WIFI_SCAN_RUNNING) {
        // Will check completion in update()
    } else if (result == WIFI_SCAN_FAILED) {
        Serial.println("[WARHOG] Scan failed");
    }
}

void WarhogMode::processScanResults() {
    int n = WiFi.scanComplete();
    
    if (n == WIFI_SCAN_RUNNING) {
        return;  // Still scanning
    }
    
    if (n == WIFI_SCAN_FAILED || n < 0) {
        Serial.println("[WARHOG] Scan failed");
        WiFi.scanDelete();
        return;
    }
    
    // Get current GPS data
    GPSData gps = GPS::getData();
    bool hasGPS = GPS::hasFix();
    
    Serial.printf("[WARHOG] Found %d networks (GPS: %s)\n", 
                 n, hasGPS ? "yes" : "no");
    
    size_t previousCount = entries.size();
    
    for (int i = 0; i < n; i++) {
        uint8_t* bssid = WiFi.BSSID(i);
        
        int idx = findEntry(bssid);
        
        if (idx < 0) {
            // Check memory limit before adding
            if (entries.size() >= MAX_ENTRIES) {
                Serial.println("[WARHOG] Max entries reached, saving and clearing");
                saveNewEntries();  // Save current entries
                entries.clear();   // Clear to make room
                previousCount = 0; // Reset so newCount calculation is correct
                // Reset stats to match cleared entries
                totalNetworks = 0;
                openNetworks = 0;
                wepNetworks = 0;
                wpaNetworks = 0;
                savedCount = 0;
            }
            
            // New network
            WardrivingEntry entry = {0};
            memcpy(entry.bssid, bssid, 6);
            strncpy(entry.ssid, WiFi.SSID(i).c_str(), 32);
            entry.rssi = WiFi.RSSI(i);
            entry.channel = WiFi.channel(i);
            entry.authmode = WiFi.encryptionType(i);
            entry.timestamp = millis();
            entry.saved = false;
            entry.label = 0;  // Unknown - user can label later
            
            // Extract ML features from scan result
            wifi_ap_record_t apRecord;
            apRecord.rssi = entry.rssi;
            apRecord.primary = entry.channel;
            apRecord.second = WIFI_SECOND_CHAN_NONE;
            apRecord.authmode = entry.authmode;
            memcpy(apRecord.bssid, bssid, 6);
            memcpy(apRecord.ssid, entry.ssid, 33);
            apRecord.phy_11n = true;  // Assume 11n capable
            entry.features = FeatureExtractor::extractFromScan(&apRecord);
            
            if (hasGPS) {
                entry.latitude = gps.latitude;
                entry.longitude = gps.longitude;
                entry.altitude = gps.altitude;
            }
            
            entries.push_back(entry);
            totalNetworks++;
            
            // Track auth types
            switch (entry.authmode) {
                case WIFI_AUTH_OPEN:
                    openNetworks++;
                    break;
                case WIFI_AUTH_WEP:
                    wepNetworks++;
                    break;
                default:
                    wpaNetworks++;
                    break;
            }
            
            Serial.printf("[WARHOG] New: %s (ch%d, %s)\n",
                         entry.ssid, entry.channel, 
                         authModeToString(entry.authmode).c_str());
        } else {
            // Update existing - maybe update GPS if we have better fix
            if (hasGPS && (entries[idx].latitude == 0 || 
                          WiFi.RSSI(i) > entries[idx].rssi)) {
                entries[idx].latitude = gps.latitude;
                entries[idx].longitude = gps.longitude;
                entries[idx].altitude = gps.altitude;
                entries[idx].rssi = WiFi.RSSI(i);
            }
        }
    }
    
    newCount = entries.size() - previousCount;
    
    if (newCount > 0) {
        // Use WARHOG-specific phrases for found networks
        Mood::onWarhogFound(nullptr, WiFi.channel());
        
        // Auto-save new entries with GPS fix to CSV
        if (hasGPS && Config::isSDAvailable()) {
            saveNewEntries();
        }
    }
    
    WiFi.scanDelete();
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
    // Create file with unique name on first save
    if (currentFilename.length() == 0) {
        currentFilename = generateFilename("csv");
        
        // Write CSV header
        File f = SD.open(currentFilename.c_str(), FILE_WRITE);
        if (f) {
            f.println("BSSID,SSID,RSSI,Channel,AuthMode,Latitude,Longitude,Altitude,Timestamp");
            f.close();
            Serial.printf("[WARHOG] Created file: %s\n", currentFilename.c_str());
        } else {
            Serial.printf("[WARHOG] Failed to create: %s\n", currentFilename.c_str());
            return;
        }
    }
    
    // Append unsaved entries that have GPS coordinates
    File f = SD.open(currentFilename.c_str(), FILE_APPEND);
    if (!f) {
        Serial.printf("[WARHOG] Failed to append to %s\n", currentFilename.c_str());
        return;
    }
    
    uint32_t newSaved = 0;
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
            newSaved++;
            savedCount++;
        }
    }
    
    f.close();
    
    if (newSaved > 0) {
        Serial.printf("[WARHOG] Saved %lu entries (total: %lu)\n", newSaved, savedCount);
    }
}

bool WarhogMode::hasGPSFix() {
    return GPS::hasFix();
}

GPSData WarhogMode::getGPSData() {
    return GPS::getData();
}

bool WarhogMode::exportCSV(const char* path) {
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[WARHOG] Failed to open %s\n", path);
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
    File f = SD.open(path, FILE_WRITE);
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
            f.printf("20%02d-%02d-%02d %02d:%02d:%02d,", year, month, day, hour, minute, second);
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
    File f = SD.open(path, FILE_WRITE);
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
    char buf[48];
    GPSData gps = GPS::getData();
    
    if (gps.date > 0 && gps.time > 0) {
        // date format: DDMMYY, time format: HHMMSSCC (centiseconds)
        uint8_t day = gps.date / 10000;
        uint8_t month = (gps.date / 100) % 100;
        uint8_t year = gps.date % 100;
        uint8_t hour = gps.time / 1000000;
        uint8_t minute = (gps.time / 10000) % 100;
        uint8_t second = (gps.time / 100) % 100;
        
        snprintf(buf, sizeof(buf), "/warhog_20%02d%02d%02d_%02d%02d%02d.%s",
                year, month, day, hour, minute, second, ext);
    } else {
        snprintf(buf, sizeof(buf), "/warhog_%lu.%s", millis(), ext);
    }
    
    return String(buf);
}

bool WarhogMode::exportMLTraining(const char* path) {
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[WARHOG] Failed to open ML export: %s\n", path);
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
