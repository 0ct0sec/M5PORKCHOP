// WPA-SEC distributed cracking service client
// https://wpa-sec.stanev.org/

#include "wpasec.h"
#include "../core/sd_layout.h"
#include "../core/config.h"
#include "../core/heap_gates.h"
#include "../core/wifi_utils.h"
#include "../core/network_recon.h"
#include "../piglet/mood.h"
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctype.h>
#include <esp_heap_caps.h>
#include <algorithm>

// WPA-SEC API
static const char* WPASEC_HOST = "wpa-sec.stanev.org";
static const uint16_t WPASEC_PORT = 443;
static const char* WPASEC_UPLOAD_PATH = "/";
static const char* WPASEC_POTFILE_PATH = "/?api&dl=1";
static const size_t WPASEC_MAX_CACHE_ENTRIES = 500;

// Static member initialization
bool WPASec::cacheLoaded = false;
char WPASec::lastError[64] = "";
std::vector<WPASec::CrackedEntry> WPASec::crackedCache;
std::vector<WPASec::UploadedEntry> WPASec::uploadedCache;
volatile bool WPASec::busy = false;
bool WPASec::batchMode = false;

bool WPASec::isBusy() {
    return busy;
}

void WPASec::normalizeBSSID_Char(const char* bssid, char* output, size_t outLen) {
    if (!bssid || !output || outLen < 1) return;
    size_t outIdx = 0;
    for (int i = 0; bssid[i] && outIdx < outLen - 1; i++) {
        char c = bssid[i];
        if (c != ':' && c != '-') {
            output[outIdx++] = (char)toupper(c);
        }
    }
    output[outIdx] = '\0';
}

// ============================================================================
// Cache Management (disk only)
// ============================================================================

bool WPASec::loadUploadedList() {
    uploadedCache.clear();
    uploadedCache.reserve(64);  // 64 * 13B = 832B — avoids 6 reallocations
    const char* uploadedPath = SDLayout::wpasecUploadedPath();
    if (!SD.exists(uploadedPath)) return true;

    File f = SD.open(uploadedPath, FILE_READ);
    if (!f) {
        strncpy(lastError, "CANNOT OPEN UPLOADED", sizeof(lastError) - 1);
        lastError[sizeof(lastError) - 1] = '\0';
        return false;
    }

    char lineBuf[64];
    while (f.available() && uploadedCache.size() < WPASEC_MAX_CACHE_ENTRIES) {
        size_t len = f.readBytesUntil('\n', lineBuf, sizeof(lineBuf) - 1);
        lineBuf[len] = '\0';
        // Trim trailing whitespace
        while (len > 0 && (lineBuf[len - 1] == '\r' || lineBuf[len - 1] == ' ')) {
            lineBuf[--len] = '\0';
        }
        if (len == 0) continue;

        UploadedEntry entry;
        normalizeBSSID_Char(lineBuf, entry.bssid, sizeof(entry.bssid));
        if (entry.bssid[0] != '\0') {
            uploadedCache.push_back(entry);
        }
    }

    f.close();
    // Sort for binary search — O(n log n) once, enables O(log n) lookups
    std::sort(uploadedCache.begin(), uploadedCache.end(),
        [](const UploadedEntry& a, const UploadedEntry& b) {
            return strcmp(a.bssid, b.bssid) < 0;
        });
    return true;
}

bool WPASec::loadCache() {
    if (cacheLoaded) return true;

    crackedCache.clear();
    crackedCache.reserve(8);  // Grow naturally — reserve(128) was 14KB contiguous
    uploadedCache.clear();

    const char* cachePath = SDLayout::wpasecResultsPath();
    if (SD.exists(cachePath)) {
        File f = SD.open(cachePath, FILE_READ);
        if (!f) {
            strncpy(lastError, "CANNOT OPEN CACHE", sizeof(lastError) - 1);
            lastError[sizeof(lastError) - 1] = '\0';
            return false;
        }

        // Format: AP_BSSID:CLIENT_BSSID:SSID:password (WPA-SEC potfile format)
        // AP_BSSID is always 12 hex chars, CLIENT_BSSID is always 12 hex chars
        // Cap at 500 entries to prevent memory exhaustion
        char lineBuf[160];
        while (f.available() && crackedCache.size() < WPASEC_MAX_CACHE_ENTRIES) {
            size_t len = f.readBytesUntil('\n', lineBuf, sizeof(lineBuf) - 1);
            lineBuf[len] = '\0';
            // Trim trailing whitespace
            while (len > 0 && (lineBuf[len - 1] == '\r' || lineBuf[len - 1] == ' ')) {
                lineBuf[--len] = '\0';
            }
            if (len == 0) continue;

            // WPA-SEC potfile: AP_BSSID:CLIENT_BSSID:SSID:password
            // Both BSSIDs are exactly 12 hex chars (no colons)
            // Password can contain colons, so we must find the THIRD colon
            // Find colons by scanning
            const char* firstColon = nullptr;
            const char* secondColon = nullptr;
            const char* thirdColon = nullptr;
            for (const char* p = lineBuf; *p; p++) {
                if (*p == ':') {
                    if (!firstColon) firstColon = p;
                    else if (!secondColon) secondColon = p;
                    else if (!thirdColon) { thirdColon = p; break; }
                }
            }

            // Validate: AP BSSID at pos 0-11 (colon at 12), client BSSID at pos 13-24 (colon at 25)
            if (firstColon && secondColon && thirdColon &&
                (firstColon - lineBuf) == 12 &&
                (secondColon - lineBuf) == 25 &&
                thirdColon > secondColon) {

                CrackedEntry entry;
                memset(&entry, 0, sizeof(entry));

                // AP BSSID: first 12 chars, normalize
                char rawBssid[13];
                memcpy(rawBssid, lineBuf, 12);
                rawBssid[12] = '\0';
                normalizeBSSID_Char(rawBssid, entry.bssid, sizeof(entry.bssid));

                // SSID: between 2nd and 3rd colon
                size_t ssidLen = (size_t)(thirdColon - secondColon - 1);
                if (ssidLen >= sizeof(entry.ssid)) ssidLen = sizeof(entry.ssid) - 1;
                memcpy(entry.ssid, secondColon + 1, ssidLen);
                entry.ssid[ssidLen] = '\0';

                // Password: everything after 3rd colon
                const char* pwStart = thirdColon + 1;
                size_t pwLen = strlen(pwStart);
                if (pwLen >= sizeof(entry.password)) pwLen = sizeof(entry.password) - 1;
                memcpy(entry.password, pwStart, pwLen);
                entry.password[pwLen] = '\0';

                crackedCache.push_back(entry);
            }
        }

        f.close();
    }

    // Sort for binary search — O(n log n) once, enables O(log n) lookups
    std::sort(crackedCache.begin(), crackedCache.end(),
        [](const CrackedEntry& a, const CrackedEntry& b) {
            return strcmp(a.bssid, b.bssid) < 0;
        });

    if (!loadUploadedList()) {
        return false;
    }

    cacheLoaded = true;
    return true;
}

// ============================================================================
// Local Cache Queries
// ============================================================================

const WPASec::CrackedEntry* WPASec::findCracked(const char* normalizedBssid) {
    auto it = std::lower_bound(crackedCache.begin(), crackedCache.end(), normalizedBssid,
        [](const CrackedEntry& entry, const char* bssid) {
            return strcmp(entry.bssid, bssid) < 0;
        });
    if (it != crackedCache.end() && strcmp(it->bssid, normalizedBssid) == 0) {
        return &(*it);
    }
    return nullptr;
}

bool WPASec::findUploaded(const char* normalizedBssid) {
    auto it = std::lower_bound(uploadedCache.begin(), uploadedCache.end(), normalizedBssid,
        [](const UploadedEntry& entry, const char* bssid) {
            return strcmp(entry.bssid, bssid) < 0;
        });
    return it != uploadedCache.end() && strcmp(it->bssid, normalizedBssid) == 0;
}

bool WPASec::isCracked(const char* bssid) {
    loadCache();
    char key[13];
    normalizeBSSID_Char(bssid, key, sizeof(key));
    return findCracked(key) != nullptr;
}

const char* WPASec::getPassword(const char* bssid) {
    loadCache();
    char key[13];
    normalizeBSSID_Char(bssid, key, sizeof(key));
    const CrackedEntry* entry = findCracked(key);
    return entry ? entry->password : "";
}

const char* WPASec::getSSID(const char* bssid) {
    loadCache();
    char key[13];
    normalizeBSSID_Char(bssid, key, sizeof(key));
    const CrackedEntry* entry = findCracked(key);
    return entry ? entry->ssid : "";
}

uint16_t WPASec::getCrackedCount() {
    loadCache();
    return crackedCache.size();
}

bool WPASec::isUploaded(const char* bssid) {
    loadCache();
    char key[13];
    normalizeBSSID_Char(bssid, key, sizeof(key));
    if (findCracked(key) != nullptr) return true;
    return findUploaded(key);
}

const char* WPASec::getLastError() {
    return lastError;
}

void WPASec::freeCacheMemory() {
    size_t crackedCount = crackedCache.size();
    size_t uploadedCount = uploadedCache.size();
    crackedCache.clear();
    crackedCache.shrink_to_fit();
    uploadedCache.clear();
    uploadedCache.shrink_to_fit();
    cacheLoaded = false;
    Serial.printf("[WPASEC] Freed cache: %u cracked, %u uploaded\n",
                  (unsigned int)crackedCount, (unsigned int)uploadedCount);
}

bool WPASec::saveUploadedList() {
    const char* uploadedPath = SDLayout::wpasecUploadedPath();
    File f = SD.open(uploadedPath, FILE_WRITE);
    if (!f) {
        strncpy(lastError, "CANNOT WRITE UPLOADED", sizeof(lastError) - 1);
        lastError[sizeof(lastError) - 1] = '\0';
        return false;
    }

    for (size_t i = 0; i < uploadedCache.size(); i++) {
        f.println(uploadedCache[i].bssid);
    }

    f.close();
    return true;
}

void WPASec::markAsUploaded(const char* bssid) {
    loadCache();
    char key[13];
    normalizeBSSID_Char(bssid, key, sizeof(key));
    if (key[0] == '\0') return;

    // Sorted insertion — maintains O(log n) lookup invariant
    auto it = std::lower_bound(uploadedCache.begin(), uploadedCache.end(), key,
        [](const UploadedEntry& e, const char* b) { return strcmp(e.bssid, b) < 0; });
    if (it != uploadedCache.end() && strcmp(it->bssid, key) == 0) return;
    if (uploadedCache.size() >= WPASEC_MAX_CACHE_ENTRIES) return;

    UploadedEntry entry;
    memcpy(entry.bssid, key, sizeof(entry.bssid));
    uploadedCache.insert(it, entry);
    if (!batchMode) {
        saveUploadedList();
    }
}

void WPASec::beginBatchUpload() {
    batchMode = true;
}

void WPASec::endBatchUpload() {
    if (batchMode) {
        batchMode = false;
        saveUploadedList();  // Single save at end of batch
        Serial.println("[WPASEC] Batch upload complete, saved uploaded list");
    }
}

// ============================================================================
// Network Operations
// ============================================================================

bool WPASec::hasApiKey() {
    const char* key = Config::wifi().wpaSecKey;
    if (!key || key[0] == '\0') return false;
    // Key should be 32 hex characters
    size_t len = strlen(key);
    if (len != 32) return false;
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit(key[i])) return false;
    }
    return true;
}

bool WPASec::canSync() {
    // Free caches to maximize available heap
    freeCacheMemory();

    HeapGates::TlsGateStatus tls = HeapGates::checkTlsGates();

    Serial.printf("[WPASEC] canSync: %u free, %u contiguous (need %u/%u)\n",
                  (unsigned int)tls.freeHeap, (unsigned int)tls.largestBlock,
                  (unsigned int)HeapPolicy::kMinHeapForTls,
                  (unsigned int)HeapPolicy::kMinContigForTls);

    return HeapGates::canTls(tls, lastError, sizeof(lastError));
}

bool WPASec::uploadSingleCapture(const char* filepath, const char* bssid) {
    if (!filepath || !bssid) return false;
    
    Serial.printf("[WPASEC] Uploading: %s\n", filepath);
    
    // Check file exists and get size
    File capFile = SD.open(filepath, FILE_READ);
    if (!capFile) {
        Serial.printf("[WPASEC] Cannot open file: %s\n", filepath);
        return false;
    }
    size_t fileSize = capFile.size();
    if (fileSize == 0 || fileSize > 100000) {  // Max 100KB
        capFile.close();
        Serial.printf("[WPASEC] Invalid file size: %u\n", (unsigned int)fileSize);
        return false;
    }
    
    // Extract filename from path
    const char* filename = strrchr(filepath, '/');
    filename = filename ? filename + 1 : filepath;
    
    // Create WiFiClientSecure with minimal buffers
    WiFiClientSecure client;
    client.setInsecure();  // Skip cert validation - saves ~10KB heap
    
    // Connect with timeout
    Serial.printf("[WPASEC] Connecting to %s:%d\n", WPASEC_HOST, WPASEC_PORT);
    if (!client.connect(WPASEC_HOST, WPASEC_PORT, 10000)) {
        capFile.close();
        char tlsErr[64] = {0};
        int errCode = client.lastError(tlsErr, sizeof(tlsErr) - 1);
        snprintf(lastError, sizeof(lastError), "TLS CONNECT: %d", errCode);
        Serial.printf("[WPASEC] TLS connect failed: err=%d (%s)\n", errCode, tlsErr);
        return false;
    }
    
    // Build multipart boundary
    char boundary[32];
    snprintf(boundary, sizeof(boundary), "----WPASec%08lX", millis());
    
    // Calculate content length
    // Multipart format:
    // --boundary\r\n
    // Content-Disposition: form-data; name="file"; filename="xxx"\r\n
    // Content-Type: application/octet-stream\r\n\r\n
    // <file data>
    // \r\n--boundary--\r\n
    char disposition[128];
    snprintf(disposition, sizeof(disposition),
             "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"",
             filename);
    
    size_t contentLength = 2 + strlen(boundary) + 2 +           // --boundary\r\n
                           strlen(disposition) + 2 +             // disposition\r\n
                           38 + 4 +                              // Content-Type + \r\n\r\n
                           fileSize +                            // file data
                           2 + 2 + strlen(boundary) + 4;         // \r\n--boundary--\r\n
    
    // Send HTTP headers
    client.printf("POST %s HTTP/1.1\r\n", WPASEC_UPLOAD_PATH);
    client.printf("Host: %s\r\n", WPASEC_HOST);
    client.printf("Cookie: key=%s\r\n", Config::wifi().wpaSecKey);
    client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
    client.printf("Content-Length: %u\r\n", (unsigned int)contentLength);
    client.print("Connection: close\r\n\r\n");
    
    // Send multipart body
    client.printf("--%s\r\n", boundary);
    client.printf("%s\r\n", disposition);
    client.print("Content-Type: application/octet-stream\r\n\r\n");
    
    // Stream file in chunks (heap-safe)
    client.setTimeout(30000);
    char chunk[256];
    size_t sent = 0;
    while (capFile.available() && sent < fileSize && client.connected()) {
        size_t toRead = min((size_t)sizeof(chunk), fileSize - sent);
        size_t bytesRead = capFile.read((uint8_t*)chunk, toRead);
        if (bytesRead > 0) {
            size_t written = client.write((uint8_t*)chunk, bytesRead);
            if (written != bytesRead) {
                capFile.close();
                snprintf(lastError, sizeof(lastError), "Write failed at %u/%u", (unsigned)sent, (unsigned)fileSize);
                return false;
            }
            sent += bytesRead;
        }
        yield();  // Let WiFi stack breathe
    }
    capFile.close();

    if (!client.connected()) {
        snprintf(lastError, sizeof(lastError), "Connection lost during upload");
        return false;
    }
    
    // Flush upload data before sending boundary
    client.flush();

    // End multipart
    client.printf("\r\n--%s--\r\n", boundary);
    
    // Read response (just check status code)
    unsigned long timeout = millis() + 10000;
    while (client.connected() && !client.available() && millis() < timeout) {
        delay(10);
        yield();  // Prevent WDT during response wait
    }
    
    bool success = false;
    if (client.available()) {
        char response[64];
        size_t len = client.readBytesUntil('\n', response, sizeof(response) - 1);
        response[len] = '\0';
        Serial.printf("[WPASEC] Response: %s\n", response);
        
        // HTTP/1.1 200 OK or similar success
        if (strstr(response, "200") || strstr(response, "201")) {
            success = true;
        } else if (strstr(response, "409")) {
            // Already uploaded - treat as success
            success = true;
            Serial.println("[WPASEC] Already uploaded (409)");
        }
    }
    
    client.stop();
    
    if (success) {
        // NOTE: Don't mark uploaded here - caller handles marking after all TLS operations
        // This avoids reloading cache during TLS when heap is tight
        Serial.printf("[WPASEC] Upload success: %s\n", bssid);
    } else {
        if (lastError[0] == '\0') {
            strncpy(lastError, "UPLOAD REJECTED", sizeof(lastError) - 1);
        }
        Serial.printf("[WPASEC] Upload error: %s (heap=%u largest=%u)\n",
                      lastError, (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
    
    return success;
}

bool WPASec::downloadPotfile(uint16_t& newCracks) {
    newCracks = 0;
    
    Serial.println("[WPASEC] Downloading potfile...");
    
    // Create WiFiClientSecure with minimal buffers
    WiFiClientSecure client;
    client.setInsecure();
    
    if (!client.connect(WPASEC_HOST, WPASEC_PORT, 10000)) {
        char tlsErr[64] = {0};
        int errCode = client.lastError(tlsErr, sizeof(tlsErr) - 1);
        snprintf(lastError, sizeof(lastError), "POTFILE TLS: %d", errCode);
        Serial.printf("[WPASEC] Potfile TLS failed: err=%d (%s)\n", errCode, tlsErr);
        return false;
    }
    
    // Send GET request
    client.printf("GET %s HTTP/1.1\r\n", WPASEC_POTFILE_PATH);
    client.printf("Host: %s\r\n", WPASEC_HOST);
    client.printf("Cookie: key=%s\r\n", Config::wifi().wpaSecKey);
    client.print("Connection: close\r\n\r\n");
    
    // Wait for response
    unsigned long timeout = millis() + 15000;
    while (client.connected() && !client.available() && millis() < timeout) {
        delay(10);
        yield();  // Prevent WDT during response wait
    }
    
    if (!client.available()) {
        client.stop();
        strncpy(lastError, "POTFILE TIMEOUT", sizeof(lastError) - 1);
        return false;
    }
    
    // Skip HTTP headers
    bool headersEnded = false;
    char headerLine[128];
    while (client.connected() && client.available() && !headersEnded) {
        size_t len = client.readBytesUntil('\n', headerLine, sizeof(headerLine) - 1);
        headerLine[len] = '\0';
        // Empty line marks end of headers
        if (len <= 1 || (len == 1 && headerLine[0] == '\r')) {
            headersEnded = true;
        }
    }
    
    if (!headersEnded) {
        client.stop();
        strncpy(lastError, "POTFILE BAD RESPONSE", sizeof(lastError) - 1);
        return false;
    }
    
    // Open cache file for writing (overwrite)
    const char* cachePath = SDLayout::wpasecResultsPath();
    File cacheFile = SD.open(cachePath, FILE_WRITE);
    if (!cacheFile) {
        client.stop();
        strncpy(lastError, "CANNOT WRITE CACHE", sizeof(lastError) - 1);
        return false;
    }
    
    // Stream potfile line-by-line directly to SD
    // Format: BSSID:SSID:password (hashcat potfile format)
    char lineBuf[160];  // Should be enough for BSSID:SSID:password
    uint16_t lineCount = 0;
    
    while (client.connected() || client.available()) {
        if (client.available()) {
            size_t len = client.readBytesUntil('\n', lineBuf, sizeof(lineBuf) - 1);
            if (len > 0) {
                lineBuf[len] = '\0';
                // Trim \r if present
                if (len > 0 && lineBuf[len - 1] == '\r') {
                    lineBuf[len - 1] = '\0';
                }
                
                // Validate line has at least 2 colons (BSSID:SSID:password)
                int colonCount = 0;
                for (size_t i = 0; lineBuf[i]; i++) {
                    if (lineBuf[i] == ':') colonCount++;
                }
                
                if (colonCount >= 2 && strlen(lineBuf) > 10) {
                    cacheFile.println(lineBuf);
                    lineCount++;
                }
            }
        } else {
            delay(10);
        }
        
        // Safety timeout
        if (millis() > timeout + 30000) {
            Serial.println("[WPASEC] Potfile download timeout");
            break;
        }
        
        yield();
    }
    
    cacheFile.close();
    client.stop();
    
    Serial.printf("[WPASEC] Potfile downloaded: %u entries\n", (unsigned int)lineCount);
    newCracks = lineCount;
    
    return true;
}

WPASecSyncResult WPASec::syncCaptures(WPASecProgressCallback cb) {
    WPASecSyncResult result = {};
    result.success = false;
    result.error[0] = '\0';
    
    busy = true;
    
    // Pause NetworkRecon - TLS operations conflict with promiscuous mode
    // conditionHeapForTLS() overrides promiscuous callbacks, breaking NetworkRecon state
    bool wasReconRunning = NetworkRecon::isRunning();
    if (wasReconRunning) {
        Serial.println("[WPASEC] Pausing NetworkRecon for TLS operations");
        NetworkRecon::pause();
        NetworkRecon::freeNetworks();  // Release ~19KB for TLS headroom
    }
    
    // Pre-flight checks
    if (!hasApiKey()) {
        strncpy(result.error, "NO WPA-SEC KEY", sizeof(result.error) - 1);
        if (wasReconRunning) NetworkRecon::resume();
        busy = false;
        return result;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        strncpy(result.error, "WIFI NOT CONNECTED", sizeof(result.error) - 1);
        if (wasReconRunning) NetworkRecon::resume();
        busy = false;
        return result;
    }

    if (cb) {
        cb("prepping heap", 0, 0);
    }
    
    // Proactive heap conditioning - condition early when heap is marginal
    // This prevents fragmentation from getting critical before TLS attempts
    HeapGates::TlsGateStatus tls = HeapGates::checkTlsGates();
    if (HeapGates::shouldProactivelyCondition(tls)) {
        if (cb) {
            cb("OPTIMIZING HEAP", 0, 0);
        }
        Serial.printf("[WPASEC] Proactive conditioning: %u < %u threshold\n",
                      (unsigned int)tls.largestBlock,
                      (unsigned int)HeapPolicy::kProactiveTlsConditioning);
        WiFiUtils::conditionHeapForTLS();
    }
    
    // Check if heap is sufficient for TLS operations
    if (!canSync()) {
        // Heap insufficient - try "OINK bounce" conditioning
        // This reclaims BLE memory and coalesces fragmented heap blocks
        if (cb) {
            cb("CONDITIONING HEAP", 0, 0);
        }
        Serial.println("[WPASEC] Heap insufficient, attempting conditioning...");
        
        size_t largestAfter = WiFiUtils::conditionHeapForTLS();
        
        // Check again after conditioning
        if (!canSync()) {
            // Still insufficient - notify user via speech balloon
            Mood::setStatusMessage("HEAP TIGHT - TRY OINK");
            snprintf(result.error, sizeof(result.error), 
                     "%s (TRY OINK)", lastError);
            if (wasReconRunning) NetworkRecon::resume();
            busy = false;
            return result;
        }
        
        Serial.printf("[WPASEC] Conditioning successful: largest=%u\n", 
                      (unsigned int)largestAfter);
    }
    
    // Collect files to upload from handshakes directory
    if (cb) {
        cb("scanning caps", 0, 0);
    }
    const char* hsDir = SDLayout::handshakesDir();
    if (!SD.exists(hsDir)) {
        strncpy(result.error, "NO HANDSHAKES DIR", sizeof(result.error) - 1);
        if (wasReconRunning) NetworkRecon::resume();
        busy = false;
        return result;
    }
    
    // First pass: count files and check which need upload
    // Only load uploaded list (not cracked cache) to avoid 14KB allocation before TLS
    loadUploadedList();
    
    // Collect pending uploads (store paths temporarily)
    struct PendingUpload {
        char path[80];
        char bssid[13];
    };
    static PendingUpload pendingUploads[16];  // Max 16 per sync (reduced from 50, saves ~3KB BSS)
    uint8_t pendingCount = 0;

    File dir = SD.open(hsDir);
    if (dir && dir.isDirectory()) {
        File file = dir.openNextFile();
        uint8_t filesScanned = 0;
        while (file && pendingCount < 16) {
            // Yield every 10 files to prevent WDT on large directories
            if (++filesScanned >= 10) {
                filesScanned = 0;
                yield();
            }
            
            const char* fname = file.name();
            size_t fnameLen = strlen(fname);
            
            // Check extension without String allocation
            bool isPCAP = (fnameLen > 5 && strcmp(fname + fnameLen - 5, ".pcap") == 0);
            bool is22000 = (fnameLen > 6 && strcmp(fname + fnameLen - 6, ".22000") == 0);
            
            if (isPCAP || is22000) {
                // Extract BSSID from filename — supports both formats:
                // Legacy: BSSID12HEX.ext (first 12 chars)
                // New:    SSID_BSSID12HEX.ext (last 12 chars of base)
                const char* dot = strrchr(fname, '.');
                size_t baseLen = dot ? (size_t)(dot - fname) : fnameLen;

                // Strip _hs suffix
                if (baseLen > 3 && strncmp(fname + baseLen - 3, "_hs", 3) == 0) {
                    baseLen -= 3;
                }

                if (baseLen >= 12) {
                    char bssid[13];

                    // Check if last 12 chars are hex and preceded by '_' (new format)
                    bool isNewFmt = false;
                    if (baseLen > 13 && fname[baseLen - 13] == '_') {
                        isNewFmt = true;
                        for (size_t i = baseLen - 12; i < baseLen; i++) {
                            char c = fname[i];
                            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
                                isNewFmt = false;
                                break;
                            }
                        }
                    }

                    if (isNewFmt) {
                        memcpy(bssid, fname + baseLen - 12, 12);
                    } else {
                        memcpy(bssid, fname, 12);
                    }
                    bssid[12] = '\0';
                    // Uppercase for consistency
                    for (int i = 0; i < 12; i++) {
                        if (bssid[i] >= 'a' && bssid[i] <= 'f') bssid[i] -= 32;
                    }
                    
                    // Check uploaded list directly (avoids 14KB crackedCache load from isUploaded)
                    char key[13];
                    normalizeBSSID_Char(bssid, key, sizeof(key));
                    bool alreadyUploaded = findUploaded(key);
                    if (!alreadyUploaded) {
                        snprintf(pendingUploads[pendingCount].path, 
                                sizeof(pendingUploads[pendingCount].path),
                                "%s/%s", hsDir, fname);
                        memcpy(pendingUploads[pendingCount].bssid, bssid, 13);
                        pendingCount++;
                    } else {
                        result.skipped++;
                    }
                }
            }
            file.close();
            file = dir.openNextFile();
        }
        if (file) file.close();  // close leftover when loop exited on pendingCount limit
        dir.close();
    }
    
    Serial.printf("[WPASEC] Found %u files to upload, %u skipped\n", 
                  (unsigned int)pendingCount, (unsigned int)result.skipped);
    
    // Free cache before TLS operations - keeps heap clear for WiFiClientSecure
    freeCacheMemory();
    
    // Track successful uploads with bitmask - avoids reloading cache during TLS
    // We mark uploaded AFTER all TLS operations complete to keep heap clear
    uint16_t successMask = 0;  // Bitmask for max 16 pending uploads

    // Upload each pending file
    if (cb) {
        cb("yoinking caps", 0, 0);
    }
    for (uint8_t i = 0; i < pendingCount; i++) {
        if (cb) {
            char status[32];
            snprintf(status, sizeof(status), "UPLOAD %u/%u", i + 1, pendingCount);
            cb(status, i + 1, pendingCount);
        }
        
        Serial.printf("[WPASEC] Heap before upload %u: %u\n",
                      i, (unsigned int)ESP.getFreeHeap());

        // Re-check TLS gates before each upload — prior uploads may have
        // fragmented heap below the contiguous block threshold
        HeapGates::TlsGateStatus tlsPerFile = HeapGates::checkTlsGates();
        if (!HeapGates::canTls(tlsPerFile, lastError, sizeof(lastError))) {
            Serial.printf("[WPASEC] Heap degraded before upload %u, aborting remaining\n", i);
            result.failed += (pendingCount - i);
            break;
        }

        if (uploadSingleCapture(pendingUploads[i].path, pendingUploads[i].bssid)) {
            result.uploaded++;
            successMask |= (1 << i);  // Track for deferred marking
        } else {
            result.failed++;
            Serial.printf("[WPASEC] Failed: %s\n", pendingUploads[i].path);
        }
        
        // Wait for LWIP async TCP cleanup before next TLS gate check
        HeapGates::waitForLwipCleanup();
    }
    
    // Mark successful uploads AFTER all TLS operations complete
    // This avoids cache reload during TLS when heap is tight
    if (result.uploaded > 0) {
        if (cb) {
            cb("marking loot", 0, 0);
        }
        loadCache();
        for (uint8_t i = 0; i < pendingCount; i++) {
            if (successMask & (1 << i)) {
                char key[13];
                normalizeBSSID_Char(pendingUploads[i].bssid, key, sizeof(key));
                // Sorted insertion — maintains binary search invariant
                auto uit = std::lower_bound(uploadedCache.begin(), uploadedCache.end(), key,
                    [](const UploadedEntry& e, const char* b) { return strcmp(e.bssid, b) < 0; });
                if (uit == uploadedCache.end() || strcmp(uit->bssid, key) != 0) {
                    if (uploadedCache.size() < WPASEC_MAX_CACHE_ENTRIES) {
                        UploadedEntry entry;
                        memcpy(entry.bssid, key, sizeof(entry.bssid));
                        uploadedCache.insert(uit, entry);
                    }
                }
            }
        }
        saveUploadedList();
        Serial.printf("[WPASEC] Marked %u uploads after TLS complete\n", result.uploaded);
    }
    
    // Download potfile
    if (cb) {
        cb("slurping potfile", 0, 0);
    }
    
    // Free any residual memory before potfile TLS
    // NOTE: We do NOT recondition heap mid-sync - that causes more fragmentation!
    // If heap was good enough to start sync, trust it. Graceful degradation if not.
    freeCacheMemory();
    delay(100);
    
    Serial.printf("[WPASEC] Heap before potfile: %u largest=%u\n", 
                  (unsigned int)ESP.getFreeHeap(),
                  (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    
    uint16_t newCracks = 0;
    bool potfileOk = false;
    
    // Attempt potfile if heap is sufficient - no reconditioning, graceful skip if low
    HeapGates::GateStatus potGate = HeapGates::checkGate(0, HeapPolicy::kMinContigForTls);
    if (potGate.failure == HeapGates::TlsGateFailure::None) {
        potfileOk = downloadPotfile(newCracks);
        if (potfileOk) {
            result.newCracked = newCracks;
            // Reload cache to get cracked count
            loadCache();
            result.cracked = crackedCache.size();
        }
    } else {
        Serial.printf("[WPASEC] Skipping potfile: insufficient heap (%u < %u)\n",
                      (unsigned int)potGate.largestBlock,
                      (unsigned int)HeapPolicy::kMinContigForTls);
        snprintf(lastError, sizeof(lastError), "POTFILE SKIP: LOW HEAP");
    }
    
    // Graceful degradation: partial success if uploads worked but potfile failed
    if (!potfileOk && result.uploaded > 0) {
        // Uploads succeeded, potfile failed - still report partial success
        snprintf(result.error, sizeof(result.error), "POTFILE: %s", lastError);
        result.success = true;  // Partial success - uploads worked
    } else if (!potfileOk) {
        strncpy(result.error, lastError, sizeof(result.error) - 1);
        result.success = (result.failed == 0);
    } else {
        result.success = (result.failed == 0);
    }
    
    // Resume NetworkRecon after sync operations complete
    if (wasReconRunning) {
        Serial.println("[WPASEC] Resuming NetworkRecon after TLS operations");
        NetworkRecon::resume();
    }
    
    busy = false;
    Serial.printf("[WPASEC] Sync complete: uploaded=%u failed=%u cracked=%u\n",
                  (unsigned int)result.uploaded, (unsigned int)result.failed,
                  (unsigned int)result.cracked);
    
    return result;
}
