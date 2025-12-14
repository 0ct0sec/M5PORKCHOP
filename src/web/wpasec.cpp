// WPA-SEC distributed cracking service client implementation

#include "wpasec.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include "../core/config.h"

// Static member initialization
bool WPASec::cacheLoaded = false;
char WPASec::lastError[64] = "";
char WPASec::statusMessage[64] = "Ready";
std::map<String, WPASec::CacheEntry> WPASec::crackedCache;
std::map<String, bool> WPASec::uploadedCache;

void WPASec::init() {
    cacheLoaded = false;
    crackedCache.clear();
    uploadedCache.clear();
    strcpy(lastError, "");
    strcpy(statusMessage, "Ready");
}

// ============================================================================
// WiFi Connection (Standalone)
// ============================================================================

bool WPASec::connect() {
    if (WiFi.status() == WL_CONNECTED) {
        strcpy(statusMessage, "Already connected");
        return true;
    }
    
    String ssid = Config::wifi().otaSSID;
    String password = Config::wifi().otaPassword;
    
    if (ssid.isEmpty()) {
        strcpy(lastError, "No WiFi SSID configured");
        strcpy(statusMessage, "No WiFi SSID");
        return false;
    }
    
    strcpy(statusMessage, "Connecting...");
    Serial.printf("[WPASEC] Connecting to %s\n", ssid.c_str());
    
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    // Wait for connection with timeout
    uint32_t startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 15000) {
        delay(100);
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        snprintf(statusMessage, sizeof(statusMessage), "IP: %s", WiFi.localIP().toString().c_str());
        Serial.printf("[WPASEC] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    
    strcpy(lastError, "Connection timeout");
    strcpy(statusMessage, "Connect failed");
    Serial.println("[WPASEC] Connection failed");
    WiFi.disconnect(true);
    return false;
}

void WPASec::disconnect() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    strcpy(statusMessage, "Disconnected");
    Serial.println("[WPASEC] Disconnected");
}

bool WPASec::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

// ============================================================================
// BSSID Normalization
// ============================================================================

String WPASec::normalizeBSSID(const char* bssid) {
    // Remove colons/dashes, convert to uppercase
    String result;
    for (int i = 0; bssid[i]; i++) {
        char c = bssid[i];
        if (c != ':' && c != '-') {
            result += (char)toupper(c);
        }
    }
    return result;
}

// ============================================================================
// Cache Management
// ============================================================================

bool WPASec::loadCache() {
    if (cacheLoaded) return true;
    
    crackedCache.clear();
    
    if (!SD.exists(CACHE_FILE)) {
        cacheLoaded = true;
        return true;  // No cache yet, that's OK
    }
    
    File f = SD.open(CACHE_FILE, FILE_READ);
    if (!f) {
        strcpy(lastError, "Cannot open cache");
        return false;
    }
    
    // Format: BSSID:SSID:password (one per line)
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.isEmpty()) continue;
        
        int firstColon = line.indexOf(':');
        int lastColon = line.lastIndexOf(':');
        
        if (firstColon > 0 && lastColon > firstColon) {
            String bssid = line.substring(0, firstColon);
            String ssid = line.substring(firstColon + 1, lastColon);
            String password = line.substring(lastColon + 1);
            
            bssid = normalizeBSSID(bssid.c_str());
            
            CacheEntry entry;
            entry.ssid = ssid;
            entry.password = password;
            crackedCache[bssid] = entry;
        }
    }
    
    f.close();
    cacheLoaded = true;
    
    // Also load uploaded list
    loadUploadedList();
    
    Serial.printf("[WPASEC] Cache loaded: %d cracked, %d uploaded\n", 
                  crackedCache.size(), uploadedCache.size());
    return true;
}

bool WPASec::saveCache() {
    File f = SD.open(CACHE_FILE, FILE_WRITE);
    if (!f) {
        strcpy(lastError, "Cannot write cache");
        return false;
    }
    
    for (auto& pair : crackedCache) {
        f.printf("%s:%s:%s\n", pair.first.c_str(), 
                 pair.second.ssid.c_str(), pair.second.password.c_str());
    }
    
    f.close();
    return true;
}

bool WPASec::loadUploadedList() {
    uploadedCache.clear();
    
    if (!SD.exists(UPLOADED_FILE)) return true;
    
    File f = SD.open(UPLOADED_FILE, FILE_READ);
    if (!f) return false;
    
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.isEmpty()) {
            uploadedCache[normalizeBSSID(line.c_str())] = true;
        }
    }
    
    f.close();
    return true;
}

bool WPASec::saveUploadedList() {
    File f = SD.open(UPLOADED_FILE, FILE_WRITE);
    if (!f) return false;
    
    for (auto& pair : uploadedCache) {
        f.println(pair.first);
    }
    
    f.close();
    return true;
}

// ============================================================================
// Local Cache Queries
// ============================================================================

bool WPASec::isCracked(const char* bssid) {
    loadCache();
    String key = normalizeBSSID(bssid);
    return crackedCache.find(key) != crackedCache.end();
}

String WPASec::getPassword(const char* bssid) {
    loadCache();
    String key = normalizeBSSID(bssid);
    auto it = crackedCache.find(key);
    if (it != crackedCache.end()) {
        return it->second.password;
    }
    return "";
}

String WPASec::getSSID(const char* bssid) {
    loadCache();
    String key = normalizeBSSID(bssid);
    auto it = crackedCache.find(key);
    if (it != crackedCache.end()) {
        return it->second.ssid;
    }
    return "";
}

uint16_t WPASec::getCrackedCount() {
    loadCache();
    return crackedCache.size();
}

bool WPASec::isUploaded(const char* bssid) {
    loadCache();
    String key = normalizeBSSID(bssid);
    // Cracked implies uploaded
    if (crackedCache.find(key) != crackedCache.end()) return true;
    return uploadedCache.find(key) != uploadedCache.end();
}

void WPASec::markUploaded(const char* bssid) {
    loadCache();
    String key = normalizeBSSID(bssid);
    uploadedCache[key] = true;
    saveUploadedList();
}

// ============================================================================
// API Operations
// ============================================================================

bool WPASec::fetchResults() {
    if (!isConnected()) {
        strcpy(lastError, "Not connected to WiFi");
        return false;
    }
    
    String key = Config::wifi().wpaSecKey;
    if (key.isEmpty()) {
        strcpy(lastError, "No WPA-SEC key configured");
        return false;
    }
    
    strcpy(statusMessage, "Fetching results...");
    Serial.println("[WPASEC] Fetching results from WPA-SEC");
    
    WiFiClientSecure client;
    client.setInsecure();  // Skip certificate validation (acceptable for this use case)
    
    HTTPClient http;
    String url = String("https://") + API_HOST + RESULTS_PATH + key;
    
    http.begin(client, url);
    http.setTimeout(30000);
    
    int httpCode = http.GET();
    
    if (httpCode != 200) {
        snprintf(lastError, sizeof(lastError), "HTTP error: %d", httpCode);
        strcpy(statusMessage, lastError);
        http.end();
        Serial.printf("[WPASEC] HTTP error: %d\n", httpCode);
        return false;
    }
    
    // Parse response: BSSID:SSID:password lines
    String response = http.getString();
    http.end();
    
    int newCracks = 0;
    int lineStart = 0;
    
    while (lineStart < (int)response.length()) {
        int lineEnd = response.indexOf('\n', lineStart);
        if (lineEnd < 0) lineEnd = response.length();
        
        String line = response.substring(lineStart, lineEnd);
        line.trim();
        lineStart = lineEnd + 1;
        
        if (line.isEmpty()) continue;
        
        // Parse BSSID:SSID:password
        // BSSID is always first 17 chars (AA:BB:CC:DD:EE:FF) or 12 chars (AABBCCDDEEFF)
        // Password is after the LAST colon
        // SSID is everything between BSSID and password (may contain colons)
        int lastColon = line.lastIndexOf(':');
        if (lastColon < 12) continue;  // Invalid line
        
        String password = line.substring(lastColon + 1);
        String bssidAndSsid = line.substring(0, lastColon);
        
        // Find where BSSID ends - it's either 17 chars (with colons) or we find 5th colon
        int colonCount = 0;
        int bssidEnd = 0;
        for (int i = 0; i < (int)bssidAndSsid.length() && colonCount < 5; i++) {
            if (bssidAndSsid[i] == ':') colonCount++;
            if (colonCount == 5) { bssidEnd = i; break; }
        }
        
        if (bssidEnd == 0 || bssidEnd >= (int)bssidAndSsid.length() - 1) continue;
        
        String bssid = bssidAndSsid.substring(0, bssidEnd);
        String ssid = bssidAndSsid.substring(bssidEnd + 1);
        
        String bssidKey = normalizeBSSID(bssid.c_str());
        
        // Check if this is new
        if (crackedCache.find(bssidKey) == crackedCache.end()) {
            newCracks++;
        }
        
        CacheEntry entry;
        entry.ssid = ssid;
        entry.password = password;
        crackedCache[bssidKey] = entry;
    }
    
    // Save updated cache
    saveCache();
    
    snprintf(statusMessage, sizeof(statusMessage), "%d cracked (%d new)", 
             crackedCache.size(), newCracks);
    Serial.printf("[WPASEC] Fetched: %d total, %d new\n", crackedCache.size(), newCracks);
    
    return true;
}

bool WPASec::uploadCapture(const char* pcapPath) {
    if (!isConnected()) {
        strcpy(lastError, "Not connected to WiFi");
        return false;
    }
    
    String key = Config::wifi().wpaSecKey;
    if (key.isEmpty()) {
        strcpy(lastError, "No WPA-SEC key configured");
        return false;
    }
    
    if (!SD.exists(pcapPath)) {
        snprintf(lastError, sizeof(lastError), "File not found");
        return false;
    }
    
    File pcapFile = SD.open(pcapPath, FILE_READ);
    if (!pcapFile) {
        strcpy(lastError, "Cannot open file");
        return false;
    }
    
    size_t fileSize = pcapFile.size();
    if (fileSize > 500000) {  // 500KB limit
        strcpy(lastError, "File too large");
        pcapFile.close();
        return false;
    }
    
    strcpy(statusMessage, "Uploading...");
    Serial.printf("[WPASEC] Uploading %s (%d bytes)\n", pcapPath, fileSize);
    
    // Read file into buffer (PCAP files are small)
    uint8_t* buffer = (uint8_t*)malloc(fileSize);
    if (!buffer) {
        strcpy(lastError, "Out of memory");
        pcapFile.close();
        return false;
    }
    
    pcapFile.read(buffer, fileSize);
    pcapFile.close();
    
    // Build multipart form data
    String boundary = "----PorkchopBoundary" + String(millis());
    
    WiFiClientSecure client;
    client.setInsecure();
    
    if (!client.connect(API_HOST, 443)) {
        strcpy(lastError, "Connection failed");
        free(buffer);
        return false;
    }
    
    // Extract filename from path
    String filename = pcapPath;
    int lastSlash = filename.lastIndexOf('/');
    if (lastSlash >= 0) {
        filename = filename.substring(lastSlash + 1);
    }
    
    // Build body parts
    String bodyStart = "--" + boundary + "\r\n";
    bodyStart += "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n";
    bodyStart += "Content-Type: application/octet-stream\r\n\r\n";
    
    String bodyEnd = "\r\n--" + boundary + "--\r\n";
    
    size_t contentLength = bodyStart.length() + fileSize + bodyEnd.length();
    
    // Send request
    client.print("POST " + String(SUBMIT_PATH) + " HTTP/1.1\r\n");
    client.print("Host: " + String(API_HOST) + "\r\n");
    client.print("Cookie: key=" + key + "\r\n");
    client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
    client.print("Content-Length: " + String(contentLength) + "\r\n");
    client.print("Connection: close\r\n\r\n");
    
    client.print(bodyStart);
    client.write(buffer, fileSize);
    client.print(bodyEnd);
    
    free(buffer);
    
    // Read response
    uint32_t timeout = millis();
    while (client.connected() && !client.available() && millis() - timeout < 10000) {
        delay(10);
    }
    
    if (!client.available()) {
        strcpy(lastError, "No response");
        client.stop();
        return false;
    }
    
    String statusLine = client.readStringUntil('\n');
    client.stop();
    
    if (statusLine.indexOf("200") > 0 || statusLine.indexOf("302") > 0) {
        strcpy(statusMessage, "Upload OK");
        Serial.println("[WPASEC] Upload successful");
        
        // Extract BSSID from filename and mark as uploaded
        String baseName = filename;
        int dotPos = baseName.indexOf('.');
        if (dotPos > 0) {
            baseName = baseName.substring(0, dotPos);
        }
        if (baseName.endsWith("_hs")) {
            baseName = baseName.substring(0, baseName.length() - 3);
        }
        markUploaded(baseName.c_str());
        
        return true;
    }
    
    snprintf(lastError, sizeof(lastError), "Upload failed: %s", statusLine.substring(0, 30).c_str());
    strcpy(statusMessage, "Upload failed");
    Serial.printf("[WPASEC] Upload failed: %s\n", statusLine.c_str());
    return false;
}

const char* WPASec::getLastError() {
    return lastError;
}

const char* WPASec::getStatus() {
    return statusMessage;
}
