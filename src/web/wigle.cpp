// WiGLE wardriving service client
// https://wigle.net/

#include "wigle.h"
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include "../core/config.h"
#include "../core/sd_layout.h"
#include "../core/heap_gates.h"
#include "../core/wifi_utils.h"
#include "../core/network_recon.h"
#include "../core/sdlog.h"
#include "../piglet/mood.h"

// Static member initialization
std::vector<WiGLE::UploadedFile> WiGLE::uploadedFiles;
bool WiGLE::listLoaded = false;
volatile bool WiGLE::busy = false;
char WiGLE::lastError[64] = "";
bool WiGLE::batchMode = false;

static const size_t WIGLE_MAX_UPLOADED = 200;

// RAII helper for busy flag
struct BusyScope {
    volatile bool& flag;
    BusyScope(volatile bool& f) : flag(f) { flag = true; }
    ~BusyScope() { flag = false; }
};

bool WiGLE::isBusy() {
    return busy;
}

const char* WiGLE::getLastError() {
    return lastError;
}

// ============================================================================
// Upload Tracking (disk only)
// ============================================================================

bool WiGLE::loadUploadedList() {
    if (listLoaded) return true;

    uploadedFiles.clear();
    uploadedFiles.reserve(WIGLE_MAX_UPLOADED);  // Full upfront reserve — no growth reallocations

    const char* uploadedPath = SDLayout::wigleUploadedPath();
    if (!SD.exists(uploadedPath)) {
        listLoaded = true;
        return true;
    }

    File f = SD.open(uploadedPath, FILE_READ);
    if (!f) return false;

    while (f.available() && uploadedFiles.size() < WIGLE_MAX_UPLOADED) {
        char buf[48];
        int n = f.readBytesUntil('\n', buf, sizeof(buf) - 1);
        buf[n] = '\0';
        // Trim trailing whitespace
        while (n > 0 && (buf[n-1] == ' ' || buf[n-1] == '\r' || buf[n-1] == '\t')) buf[--n] = '\0';
        if (n > 0) {
            UploadedFile entry;
            strncpy(entry.name, buf, sizeof(entry.name) - 1);
            entry.name[sizeof(entry.name) - 1] = '\0';
            uploadedFiles.push_back(entry);
        }
    }

    f.close();
    listLoaded = true;
    Serial.printf("[WIGLE] Loaded %d uploaded files from tracking\n", uploadedFiles.size());
    return true;
}

bool WiGLE::saveUploadedList() {
    const char* uploadedPath = SDLayout::wigleUploadedPath();
    File f = SD.open(uploadedPath, FILE_WRITE);
    if (!f) return false;

    for (const auto& entry : uploadedFiles) {
        f.println(entry.name);
    }

    f.close();
    return true;
}

void WiGLE::freeUploadedListMemory() {
    size_t count = uploadedFiles.size();
    uploadedFiles.clear();
    uploadedFiles.shrink_to_fit();
    listLoaded = false;
    Serial.printf("[WIGLE] Freed uploaded list: %u entries\n", (unsigned int)count);
}

const char* WiGLE::getFilenameFromPath(const char* path) {
    if (!path) return "";
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool WiGLE::isUploaded(const char* filename) {
    if (!filename) return false;
    loadUploadedList();

    const char* baseName = getFilenameFromPath(filename);

    for (const auto& entry : uploadedFiles) {
        if (strcmp(entry.name, filename) == 0 || strcmp(entry.name, baseName) == 0) return true;
    }
    return false;
}

void WiGLE::markAsUploaded(const char* filename) {
    if (!filename) return;
    loadUploadedList();

    const char* baseName = getFilenameFromPath(filename);

    // Check if already in list
    for (const auto& entry : uploadedFiles) {
        if (strcmp(entry.name, baseName) == 0) return;
    }

    if (uploadedFiles.size() >= WIGLE_MAX_UPLOADED) {
        // Cap in-memory list to avoid unbounded heap growth.
        return;
    }
    UploadedFile entry;
    strncpy(entry.name, baseName, sizeof(entry.name) - 1);
    entry.name[sizeof(entry.name) - 1] = '\0';
    uploadedFiles.push_back(entry);
    if (!batchMode) {
        saveUploadedList();  // Only save immediately if not in batch mode
    }
}

void WiGLE::beginBatchUpload() {
    batchMode = true;
}

void WiGLE::endBatchUpload() {
    if (batchMode) {
        batchMode = false;
        saveUploadedList();  // Single save at end of batch
        Serial.println("[WIGLE] Batch upload complete, saved uploaded list");
    }
}

void WiGLE::removeFromUploaded(const char* filename) {
    if (!filename) return;
    loadUploadedList();

    const char* baseName = getFilenameFromPath(filename);

    bool changed = false;
    for (auto it = uploadedFiles.begin(); it != uploadedFiles.end(); ) {
        if (strcmp(it->name, filename) == 0 || strcmp(it->name, baseName) == 0) {
            it = uploadedFiles.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    if (changed) {
        saveUploadedList();
    }
}

uint16_t WiGLE::getUploadedCount() {
    loadUploadedList();
    return uploadedFiles.size();
}

// ============================================================================
// Cached User Stats (no network)
// ============================================================================

WiGLE::WigleUserStats WiGLE::getUserStats() {
    WigleUserStats stats;
    stats.valid = false;

    if (!Config::isSDAvailable()) {
        return stats;
    }

    const char* statsPath = SDLayout::wigleStatsPath();
    File f = SD.open(statsPath, FILE_READ);
    if (!f) {
        return stats;
    }

    size_t size = f.size();
    if (size == 0 || size > 512) {
        f.close();
        return stats;
    }

    char buf[512] = {0};
    size_t readLen = f.readBytes(buf, sizeof(buf) - 1);
    buf[readLen] = '\0';
    f.close();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf, readLen);
    if (err) {
        return stats;
    }

    stats.rank = doc["rank"].as<int64_t>();
    stats.wifi = doc["wifi"].as<uint64_t>();
    stats.cell = doc["cell"].as<uint64_t>();
    stats.bt = doc["bt"].as<uint64_t>();
    stats.valid = true;
    return stats;
}

// ============================================================================
// Network Operations
// ============================================================================

bool WiGLE::hasCredentials() {
    const char* name = Config::wifi().wigleApiName;
    const char* token = Config::wifi().wigleApiToken;
    
    if (!name || name[0] == '\0') return false;
    if (!token || token[0] == '\0') return false;
    
    return true;
}

bool WiGLE::canSync() {
    // Free uploaded list to maximize available heap
    freeUploadedListMemory();

    HeapGates::TlsGateStatus tls = HeapGates::checkTlsGates();

    Serial.printf("[WIGLE] canSync: %u free, %u contiguous (need %u/%u)\n",
                  (unsigned int)tls.freeHeap, (unsigned int)tls.largestBlock,
                  (unsigned int)HeapPolicy::kMinHeapForTls,
                  (unsigned int)HeapPolicy::kMinContigForTls);

    return HeapGates::canTls(tls, lastError, sizeof(lastError));
}

bool WiGLE::uploadSingleFile(const char* csvPath) {
    if (!csvPath) return false;
    
    Serial.printf("[WIGLE] Uploading: %s\n", csvPath);
    
    // Check file exists and get size
    File csvFile = SD.open(csvPath, FILE_READ);
    if (!csvFile) {
        Serial.printf("[WIGLE] Cannot open file: %s\n", csvPath);
        return false;
    }
    
    size_t fileSize = csvFile.size();
    if (fileSize == 0 || fileSize > 500000) {  // Max 500KB for ESP32 memory safety
        csvFile.close();
        Serial.printf("[WIGLE] Invalid file size: %u\n", (unsigned int)fileSize);
        strncpy(lastError, "FILE TOO LARGE", sizeof(lastError) - 1);
        return false;
    }
    
    // Extract filename from path (use strrchr instead of String)
    const char* filename = strrchr(csvPath, '/');
    filename = filename ? filename + 1 : csvPath;

    // Build Basic Auth header on stack — no heap allocation during TLS window
    char credBuf[132];  // wigleApiName(64) + ":" + wigleApiToken(64) + NUL
    snprintf(credBuf, sizeof(credBuf), "%s:%s",
             Config::wifi().wigleApiName, Config::wifi().wigleApiToken);
    char b64Buf[180];   // ceil(132/3)*4 = 176 + NUL
    size_t b64Len = 0;
    mbedtls_base64_encode((unsigned char*)b64Buf, sizeof(b64Buf), &b64Len,
                          (const unsigned char*)credBuf, strlen(credBuf));
    char authHeader[192];  // "Basic " + b64 + NUL
    snprintf(authHeader, sizeof(authHeader), "Basic %s", b64Buf);

    // Create WiFiClientSecure with minimal buffers
    WiFiClientSecure client;
    client.setInsecure();  // Skip cert validation - saves ~10KB heap
    // NOTE: setNoDelay()/setTimeout() before connect() causes EBADF errors
    // Socket doesn't exist yet - those calls require an active socket

    // Connect with timeout (15s)
    Serial.printf("[WIGLE] Connecting to %s:%d\n", API_HOST, API_PORT);
    if (!client.connect(API_HOST, API_PORT, 15000)) {
        csvFile.close();
        // Capture mbedTLS error for diagnostics
        char tlsErr[64] = {0};
        int errCode = client.lastError(tlsErr, sizeof(tlsErr) - 1);
        snprintf(lastError, sizeof(lastError), "TLS CONNECT: %d", errCode);
        Serial.printf("[WIGLE] TLS connect failed: err=%d (%s)\n", errCode, tlsErr);
        return false;
    }
    
    // Set timeout for upload operations (30s - uploads can be slow)
    client.setTimeout(30000);
    
    // Build multipart boundary
    char boundary[48];
    snprintf(boundary, sizeof(boundary), "----PorkchopWiGLE%08lX", millis());
    
    // FIX: Use stack buffers for multipart body to avoid String concatenation fragmentation
    // bodyStart: "--boundary\r\nContent-Disposition: ...; filename="name"\r\nContent-Type: text/csv\r\n\r\n"
    // Max ~200 bytes with reasonable filename
    char bodyStart[220];
    int bodyStartLen = snprintf(bodyStart, sizeof(bodyStart),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: text/csv\r\n\r\n",
        boundary, filename);
    
    // bodyEnd: "\r\n--boundary--\r\n" (~60 bytes max)
    char bodyEnd[64];
    int bodyEndLen = snprintf(bodyEnd, sizeof(bodyEnd), "\r\n--%s--\r\n", boundary);
    
    size_t contentLength = bodyStartLen + fileSize + bodyEndLen;
    
    // Send HTTP headers
    client.printf("POST %s HTTP/1.1\r\n", UPLOAD_PATH);
    client.printf("Host: %s\r\n", API_HOST);
    client.print("Authorization: ");
    client.print(authHeader);
    client.print("\r\n");
    client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
    client.printf("Content-Length: %u\r\n", (unsigned int)contentLength);
    client.print("Connection: close\r\n\r\n");
    
    // Send multipart body start
    client.print(bodyStart);
    
    // Stream file in chunks (heap-safe, 2KB for fewer TLS operations)
    const size_t CHUNK_SIZE = 2048;
    uint8_t chunk[CHUNK_SIZE];
    size_t bytesRemaining = fileSize;
    size_t bytesSent = 0;
    
    while (bytesRemaining > 0) {
        // Verify connection is still alive before each chunk
        if (!client.connected()) {
            char tlsErr[64] = {0};
            int errCode = client.lastError(tlsErr, sizeof(tlsErr) - 1);
            snprintf(lastError, sizeof(lastError), "CONN LOST @%uB: %d", 
                     (unsigned int)bytesSent, errCode);
            Serial.printf("[WIGLE] Connection lost during upload: sent=%u/%u, err=%d (%s)\n",
                          (unsigned int)bytesSent, (unsigned int)fileSize, errCode, tlsErr);
            csvFile.close();
            client.stop();
            return false;
        }
        
        size_t toRead = (bytesRemaining > CHUNK_SIZE) ? CHUNK_SIZE : bytesRemaining;
        size_t bytesRead = csvFile.read(chunk, toRead);
        if (bytesRead == 0) {
            snprintf(lastError, sizeof(lastError), "SD READ @%uB", (unsigned int)bytesSent);
            Serial.printf("[WIGLE] SD read failed at offset %u/%u\n", 
                          (unsigned int)bytesSent, (unsigned int)fileSize);
            csvFile.close();
            client.stop();
            return false;
        }
        
        size_t written = client.write(chunk, bytesRead);
        if (written != bytesRead) {
            char tlsErr[64] = {0};
            int errCode = client.lastError(tlsErr, sizeof(tlsErr) - 1);
            snprintf(lastError, sizeof(lastError), "TLS WRITE: %d @%uB", 
                     errCode, (unsigned int)bytesSent);
            Serial.printf("[WIGLE] TLS write failed: wrote=%u/%u, sent=%u/%u, err=%d (%s), conn=%d\n",
                          (unsigned int)written, (unsigned int)bytesRead,
                          (unsigned int)bytesSent, (unsigned int)fileSize,
                          errCode, tlsErr, client.connected());
            csvFile.close();
            client.stop();
            return false;
        }
        
        bytesSent += bytesRead;
        bytesRemaining -= bytesRead;
        yield();  // Let WiFi stack breathe
    }
    csvFile.close();
    
    // Send multipart body end
    client.print(bodyEnd);
    client.flush();
    
    // Read response
    unsigned long timeout = millis() + 15000;
    while (client.connected() && !client.available() && millis() < timeout) {
        delay(10);
        yield();
    }
    
    // Parse HTTP status code
    int statusCode = 0;
    if (client.available()) {
        char statusLine[64];
        size_t sLen = client.readBytesUntil('\n', statusLine, sizeof(statusLine) - 1);
        statusLine[sLen] = '\0';
        // Parse "HTTP/1.1 200 OK"
        const char* sp = strchr(statusLine, ' ');
        if (sp) statusCode = atoi(sp + 1);
    }

    // Skip headers
    {
        char hBuf[128];
        while (client.connected()) {
            size_t hLen = client.readBytesUntil('\n', hBuf, sizeof(hBuf) - 1);
            hBuf[hLen] = '\0';
            if (hLen <= 1) break;  // Empty line or just \r
        }
    }
    
    // Read response body (for error context)
    // FIX: Use stack buffer to avoid heap fragmentation from char-by-char concat
    char body[260];
    size_t bodyLen = 0;
    unsigned long bodyTimeout = millis() + 5000;
    while ((client.connected() || client.available()) && 
           bodyLen < sizeof(body) - 1 && millis() < bodyTimeout) {
        if (client.available()) {
            body[bodyLen++] = (char)client.read();
        } else {
            delay(1);
        }
    }
    body[bodyLen] = '\0';
    
    client.stop();
    
    // Check for success
    bool success = false;
    if (statusCode == 200 || statusCode == 302) {
        // Parse JSON response to check success field
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);
        if (!err && doc["success"].as<bool>() == true) {
            success = true;
        } else if (statusCode == 200) {
            // Some older API versions don't return JSON
            success = true;
        }
    }
    
    if (success) {
        // NOTE: Don't mark uploaded here - caller handles marking after all TLS operations
        // This avoids reloading list during TLS when heap is tight
        Serial.printf("[WIGLE] Upload success: %s\n", csvPath);
        SDLog::log("WIGLE", "Upload OK: %s", filename);
        return true;
    }
    
    // Build error message
    if (statusCode > 0) {
        snprintf(lastError, sizeof(lastError), "HTTP %d", statusCode);
    } else {
        strncpy(lastError, "NO RESPONSE", sizeof(lastError) - 1);
    }
    
    Serial.printf("[WIGLE] Upload failed: %s - %s\n", csvPath, lastError);
    SDLog::log("WIGLE", "Upload failed: %s", filename);
    return false;
}

bool WiGLE::fetchStats() {
    Serial.println("[WIGLE] Fetching user stats...");
    
    // Build Basic Auth header on stack — no heap allocation during TLS window
    char credBuf[132];
    snprintf(credBuf, sizeof(credBuf), "%s:%s",
             Config::wifi().wigleApiName, Config::wifi().wigleApiToken);
    char b64Buf[180];
    size_t b64Len = 0;
    mbedtls_base64_encode((unsigned char*)b64Buf, sizeof(b64Buf), &b64Len,
                          (const unsigned char*)credBuf, strlen(credBuf));
    char authHeader[192];
    snprintf(authHeader, sizeof(authHeader), "Basic %s", b64Buf);
    
    // Create WiFiClientSecure
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(30000);
    
    if (!client.connect(API_HOST, API_PORT, 10000)) {
        strncpy(lastError, "STATS TLS FAILED", sizeof(lastError) - 1);
        Serial.println("[WIGLE] Stats TLS connection failed");
        return false;
    }
    
    // Send GET request
    client.printf("GET %s HTTP/1.1\r\n", STATS_PATH);
    client.printf("Host: %s\r\n", API_HOST);
    client.print("Authorization: ");
    client.print(authHeader);
    client.print("\r\n");
    client.print("Connection: close\r\n\r\n");
    
    // Wait for response
    unsigned long timeout = millis() + 15000;
    while (client.connected() && !client.available() && millis() < timeout) {
        delay(10);
        yield();
    }
    
    // Read status code
    int statusCode = 0;
    if (client.available()) {
        char statusLine[64];
        size_t sLen = client.readBytesUntil('\n', statusLine, sizeof(statusLine) - 1);
        statusLine[sLen] = '\0';
        const char* sp = strchr(statusLine, ' ');
        if (sp) statusCode = atoi(sp + 1);
    }

    if (statusCode != 200) {
        client.stop();
        snprintf(lastError, sizeof(lastError), "STATS HTTP %d", statusCode);
        return false;
    }
    
    // Skip headers
    {
        char hBuf[128];
        while (client.connected()) {
            size_t hLen = client.readBytesUntil('\n', hBuf, sizeof(hBuf) - 1);
            hBuf[hLen] = '\0';
            if (hLen <= 1) break;
        }
    }
    
    // Read JSON body
    // FIX: Use stack buffer to avoid heap fragmentation from char-by-char concat
    char body[2050];
    size_t bodyLen = 0;
    unsigned long bodyTimeout = millis() + 10000;
    while ((client.connected() || client.available()) && 
           bodyLen < sizeof(body) - 1 && millis() < bodyTimeout) {
        if (client.available()) {
            body[bodyLen++] = (char)client.read();
        } else {
            delay(1);
        }
    }
    body[bodyLen] = '\0';
    
    client.stop();
    
    // Parse JSON
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body, bodyLen);
    if (err) {
        strncpy(lastError, "STATS JSON ERROR", sizeof(lastError) - 1);
        return false;
    }
    
    // Extract stats (handle different API response formats)
    JsonDocument statsDoc;
    statsDoc["rank"] = doc["rank"] | (doc["statistics"]["rank"] | 0);
    
    JsonObject stats = doc["statistics"].as<JsonObject>();
    if (stats) {
        statsDoc["wifi"] = stats["discoveredWiFi"] | stats["wifiCount"] | 0;
        statsDoc["cell"] = stats["discoveredCell"] | stats["cellCount"] | 0;
        statsDoc["bt"] = stats["discoveredBt"] | stats["btCount"] | 0;
    } else {
        statsDoc["wifi"] = 0;
        statsDoc["cell"] = 0;
        statsDoc["bt"] = 0;
    }
    
    // Save to SD card
    const char* statsPath = SDLayout::wigleStatsPath();
    File f = SD.open(statsPath, FILE_WRITE);
    if (!f) {
        strncpy(lastError, "CANNOT SAVE STATS", sizeof(lastError) - 1);
        return false;
    }
    
    serializeJson(statsDoc, f);
    f.close();
    
    Serial.println("[WIGLE] Stats saved successfully");
    SDLog::log("WIGLE", "Stats fetched: rank=%lld", doc["rank"].as<int64_t>());
    return true;
}

WigleSyncResult WiGLE::syncFiles(WigleProgressCallback cb) {
    WigleSyncResult result = {};
    result.success = false;
    result.error[0] = '\0';
    
    busy = true;
    
    // Pause NetworkRecon - TLS operations conflict with promiscuous mode
    // conditionHeapForTLS() overrides promiscuous callbacks, breaking NetworkRecon state
    bool wasReconRunning = NetworkRecon::isRunning();
    if (wasReconRunning) {
        Serial.println("[WIGLE] Pausing NetworkRecon for TLS operations");
        NetworkRecon::pause();
    }
    
    // Pre-flight checks
    if (!hasCredentials()) {
        strncpy(result.error, "NO WIGLE CREDENTIALS", sizeof(result.error) - 1);
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
        Serial.printf("[WIGLE] Proactive conditioning: %u < %u threshold\n",
                      (unsigned int)tls.largestBlock,
                      (unsigned int)HeapPolicy::kProactiveTlsConditioning);
        WiFiUtils::conditionHeapForTLS();
    }
    
    // Check if heap is sufficient for TLS operations
    if (!canSync()) {
        // Heap insufficient - try conditioning
        if (cb) {
            cb("CONDITIONING HEAP", 0, 0);
        }
        Serial.println("[WIGLE] Heap insufficient, attempting conditioning...");
        
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
        
        Serial.printf("[WIGLE] Conditioning successful: largest=%u\n", 
                      (unsigned int)largestAfter);
    }
    
    // Collect files to upload from wardriving directory
    if (cb) {
        cb("scanning csv", 0, 0);
    }
    const char* wardrivingDir = SDLayout::wardrivingDir();
    if (!SD.exists(wardrivingDir)) {
        strncpy(result.error, "NO WARDRIVING DIR", sizeof(result.error) - 1);
        if (wasReconRunning) NetworkRecon::resume();
        busy = false;
        return result;
    }
    
    // First pass: count files and check which need upload
    loadUploadedList();
    
    // Collect pending uploads
    struct PendingUpload {
        char path[80];
    };
    static PendingUpload pendingUploads[16];  // Max 16 per sync (reduced from 50, saves ~2.7KB BSS)
    uint8_t pendingCount = 0;

    File dir = SD.open(wardrivingDir);
    if (dir && dir.isDirectory()) {
        File file = dir.openNextFile();
        uint8_t filesScanned = 0;
        while (file && pendingCount < 16) {
            // Yield every 10 files to prevent WDT
            if (++filesScanned >= 10) {
                filesScanned = 0;
                yield();
            }
            
            const char* fname = file.name();
            size_t fnameLen = strlen(fname);
            
            // Check for WiGLE CSV files
            bool isWigleCSV = (fnameLen > 10 && strstr(fname, ".wigle.csv") != nullptr);
            
            if (isWigleCSV) {
                // Check if already uploaded
                char fullPath[80];
                snprintf(fullPath, sizeof(fullPath), "%s/%s", wardrivingDir, fname);
                if (!isUploaded(fullPath)) {
                    snprintf(pendingUploads[pendingCount].path, 
                            sizeof(pendingUploads[pendingCount].path),
                            "%s/%s", wardrivingDir, fname);
                    pendingCount++;
                } else {
                    result.skipped++;
                }
            }
            file.close();
            file = dir.openNextFile();
        }
        if (file) file.close();  // close leftover when loop exited on pendingCount limit
        dir.close();
    }

    Serial.printf("[WIGLE] Found %u files to upload, %u skipped\n",
                  (unsigned int)pendingCount, (unsigned int)result.skipped);
    
    // Free memory before TLS operations - keeps heap clear for WiFiClientSecure
    freeUploadedListMemory();
    
    // Track successful uploads with bitmask - avoids reloading list during TLS
    // We mark uploaded AFTER all TLS operations complete to keep heap clear
    uint8_t successMask[50] = {0};
    
    // Upload each pending file
    if (cb) {
        cb("uploading wigle", 0, 0);
    }
    for (uint8_t i = 0; i < pendingCount; i++) {
        if (cb) {
            char status[32];
            snprintf(status, sizeof(status), "UPLOAD %u/%u", i + 1, pendingCount);
            cb(status, i + 1, pendingCount);
        }
        
        Serial.printf("[WIGLE] Heap before upload %u: %u\n", 
                      i, (unsigned int)ESP.getFreeHeap());
        
        if (uploadSingleFile(pendingUploads[i].path)) {
            result.uploaded++;
            successMask[i] = 1;  // Track for deferred marking
        } else {
            result.failed++;
            Serial.printf("[WIGLE] Failed: %s\n", pendingUploads[i].path);
        }
        
        // Small delay between uploads to let heap settle
        delay(100);
        yield();
    }
    
    // Mark successful uploads AFTER all TLS operations complete
    // This avoids list reload during TLS when heap is tight
    if (result.uploaded > 0) {
        if (cb) {
            cb("marking uploads", 0, 0);
        }
        loadUploadedList();
        for (uint8_t i = 0; i < pendingCount; i++) {
            if (successMask[i]) {
                const char* baseName = getFilenameFromPath(pendingUploads[i].path);
                // Check if already in list (avoid duplicates)
                bool found = false;
                for (const auto& entry : uploadedFiles) {
                    if (strcmp(entry.name, baseName) == 0) { found = true; break; }
                }
                if (!found && uploadedFiles.size() < WIGLE_MAX_UPLOADED) {
                    UploadedFile entry;
                    strncpy(entry.name, baseName, sizeof(entry.name) - 1);
                    entry.name[sizeof(entry.name) - 1] = '\0';
                    uploadedFiles.push_back(entry);
                }
            }
        }
        saveUploadedList();
        Serial.printf("[WIGLE] Marked %u uploads after TLS complete\n", result.uploaded);
    }
    
    // Fetch stats after uploads
    if (cb) {
        cb("slurping stats", 0, 0);
    }
    
    // Free residual memory before stats TLS
    // NOTE: We do NOT recondition heap mid-sync - that causes more fragmentation!
    // If heap was good enough to start sync, trust it. Stats fetch is optional anyway.
    freeUploadedListMemory();
    delay(100);
    
    Serial.printf("[WIGLE] Heap before stats: %u largest=%u\n", 
                  (unsigned int)ESP.getFreeHeap(),
                  (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    
    // Attempt stats fetch if heap sufficient - no reconditioning, graceful skip if low
    HeapGates::GateStatus statsGate = HeapGates::checkGate(0, HeapPolicy::kMinContigForTls);
    if (statsGate.failure == HeapGates::TlsGateFailure::None) {
        result.statsFetched = fetchStats();
        if (!result.statsFetched) {
            Serial.printf("[WIGLE] Stats fetch failed: %s\n", lastError);
        }
    } else {
        Serial.println("[WIGLE] Skipping stats - heap too low");
        result.statsFetched = false;
    }
    
    // Determine overall success
    if (result.uploaded > 0 || (pendingCount == 0 && result.skipped > 0)) {
        result.success = true;
    } else if (pendingCount == 0) {
        result.success = true;  // Nothing to upload is still success
    } else if (result.failed == pendingCount) {
        strncpy(result.error, lastError, sizeof(result.error) - 1);
    }
    
    // Resume NetworkRecon after sync operations complete
    if (wasReconRunning) {
        Serial.println("[WIGLE] Resuming NetworkRecon after TLS operations");
        NetworkRecon::resume();
    }
    
    busy = false;
    
    Serial.printf("[WIGLE] Sync complete: up=%u fail=%u skip=%u stats=%s\n",
                  (unsigned int)result.uploaded, (unsigned int)result.failed,
                  (unsigned int)result.skipped, result.statsFetched ? "yes" : "no");
    
    return result;
}
