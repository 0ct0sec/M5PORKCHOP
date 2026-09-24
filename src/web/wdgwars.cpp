#include "wdgwars.h"
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <ctype.h>
#include "../core/config.h"
#include "../core/sd_layout.h"
#include "../core/heap_gates.h"
#include "../core/wifi_utils.h"
#include "../core/network_recon.h"
#include "../core/sdlog.h"
#include "../piglet/mood.h"

std::vector<WDGWars::UploadedFile> WDGWars::uploadedFiles;
bool WDGWars::listLoaded = false;
char WDGWars::lastError[64] = "";
int WDGWars::lastHttpStatus = 0;
uint16_t WDGWars::retryAfterSeconds = 0;

namespace {
static constexpr size_t WDG_MIN_FREE_FOR_TLS = 50U * 1024U;
static constexpr size_t WDG_MIN_CONTIG_FOR_TLS = 30U * 1024U;

const char* baseName(const char* p) {
    const char* slash = p ? strrchr(p, '/') : nullptr;
    return slash ? slash + 1 : (p ? p : "");
}
bool wigleCsv(const char* name) {
    size_t n = name ? strlen(name) : 0;
    return n > 10 && strcasecmp(name + n - 10, ".wigle.csv") == 0;
}
}

bool WDGWars::hasCredentials() {
    const char* key = Config::wifi().wdgWarsApiKey;
    if (!key || strlen(key) != 64) return false;
    for (size_t i = 0; i < 64; ++i) if (!isxdigit((unsigned char)key[i])) return false;
    return true;
}

bool WDGWars::loadUploadedList() {
    if (listLoaded) return true;
    uploadedFiles.clear();
    uploadedFiles.reserve(8);
    File f = SD.open(SDLayout::wdgWarsUploadedPath(), FILE_READ);
    if (f) {
        while (f.available() && uploadedFiles.size() < 200) {
            UploadedFile item = {};
            int n = f.readBytesUntil('\n', item.name, sizeof(item.name) - 1);
            while (n && (item.name[n - 1] == '\r' || item.name[n - 1] == ' ')) item.name[--n] = '\0';
            if (n) uploadedFiles.push_back(item);
        }
        f.close();
    }
    std::sort(uploadedFiles.begin(), uploadedFiles.end(), [](const UploadedFile& a, const UploadedFile& b) {
        return strcmp(a.name, b.name) < 0;
    });
    listLoaded = true;
    return true;
}

bool WDGWars::saveUploadedList() {
    File f = SD.open(SDLayout::wdgWarsUploadedPath(), FILE_WRITE);
    if (!f) return false;
    for (const auto& item : uploadedFiles) f.println(item.name);
    f.close();
    return true;
}

bool WDGWars::isUploaded(const char* filename) {
    loadUploadedList();
    const char* name = baseName(filename);
    auto it = std::lower_bound(uploadedFiles.begin(), uploadedFiles.end(), name,
        [](const UploadedFile& item, const char* value) { return strcmp(item.name, value) < 0; });
    return it != uploadedFiles.end() && strcmp(it->name, name) == 0;
}

void WDGWars::markAsUploaded(const char* filename) {
    loadUploadedList();
    const char* name = baseName(filename);
    auto it = std::lower_bound(uploadedFiles.begin(), uploadedFiles.end(), name,
        [](const UploadedFile& item, const char* value) { return strcmp(item.name, value) < 0; });
    if (it != uploadedFiles.end() && strcmp(it->name, name) == 0) return;
    if (uploadedFiles.size() >= 200) return;
    UploadedFile item = {};
    strncpy(item.name, name, sizeof(item.name) - 1);
    uploadedFiles.insert(it, item);
}

void WDGWars::removeFromUploaded(const char* filename) {
    loadUploadedList();
    const char* name = baseName(filename);
    auto it = std::lower_bound(uploadedFiles.begin(), uploadedFiles.end(), name,
        [](const UploadedFile& item, const char* value) { return strcmp(item.name, value) < 0; });
    if (it != uploadedFiles.end() && strcmp(it->name, name) == 0) {
        uploadedFiles.erase(it);
        saveUploadedList();
    }
}

void WDGWars::freeUploadedListMemory() {
    uploadedFiles.clear();
    uploadedFiles.shrink_to_fit();
    listLoaded = false;
}

bool WDGWars::uploadSingleFile(const char* path) {
    lastHttpStatus = 0;
    retryAfterSeconds = 0;
    File csv = SD.open(path, FILE_READ);
    if (!csv) { strncpy(lastError, "CANNOT OPEN FILE", sizeof(lastError) - 1); return false; }
    size_t fileSize = csv.size();
    if (!fileSize || fileSize > 4U * 1024U * 1024U) {
        csv.close(); strncpy(lastError, "FILE TOO LARGE", sizeof(lastError) - 1); return false;
    }

    IPAddress resolvedIp;
    if (!WiFi.hostByName("wdgwars.pl", resolvedIp)) {
        csv.close();
        strncpy(lastError, "DNS LOOKUP FAILED", sizeof(lastError) - 1);
        Serial.printf("[WDGWARS] DNS failed; WiFi=%d RSSI=%d\n",
                      (int)WiFi.status(), WiFi.RSSI());
        return false;
    }
    Serial.printf("[WDGWARS] Resolved wdgwars.pl to %s\n", resolvedIp.toString().c_str());

    WiFiClientSecure client;
    client.setInsecure();
    if (!client.connect("wdgwars.pl", 443, 15000)) {
        char tlsText[48] = {};
        int tlsCode = client.lastError(tlsText, sizeof(tlsText) - 1);
        size_t freeHeap = ESP.getFreeHeap();
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        csv.close();
        snprintf(lastError, sizeof(lastError), "TLS %d %u/%uKB", tlsCode,
                 (unsigned)(largest / 1024), (unsigned)(freeHeap / 1024));
        Serial.printf("[WDGWARS] TLS connect failed: code=%d (%s), WiFi=%d, free=%u, largest=%u\n",
                      tlsCode, tlsText, (int)WiFi.status(), (unsigned)freeHeap, (unsigned)largest);
        client.stop();
        return false;
    }
    client.setTimeout(30000);
    char boundary[48];
    snprintf(boundary, sizeof(boundary), "----PorkchopWDG%08lX", millis());
    char start[220];
    int startLen = snprintf(start, sizeof(start),
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: text/csv\r\n\r\n", boundary, baseName(path));
    char end[64];
    int endLen = snprintf(end, sizeof(end), "\r\n--%s--\r\n", boundary);
    size_t contentLength = (size_t)startLen + fileSize + (size_t)endLen;
    client.print("POST /api/upload-csv HTTP/1.1\r\nHost: wdgwars.pl\r\nX-API-Key: ");
    client.print(Config::wifi().wdgWarsApiKey);
    client.printf("\r\nContent-Type: multipart/form-data; boundary=%s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                  boundary, (unsigned)contentLength);
    client.print(start);
    // Keep the loopTask stack shallow during mbedTLS's deep handshake/write
    // path. A 2KB local buffer contributed to an 8KB stack-watchpoint panic.
    uint8_t chunk[512];
    while (csv.available()) {
        size_t got = csv.read(chunk, sizeof(chunk));
        if (!got || client.write(chunk, got) != got) {
            csv.close(); client.stop(); strncpy(lastError, "UPLOAD WRITE FAILED", sizeof(lastError) - 1); return false;
        }
        yield();
    }
    csv.close();
    client.print(end);
    client.flush();
    unsigned long deadline = millis() + 20000;
    while (!client.available() && client.connected() && millis() < deadline) { delay(10); yield(); }
    char line[96] = {};
    int status = 0;
    if (client.available()) {
        size_t len = client.readBytesUntil('\n', line, sizeof(line) - 1);
        line[len] = '\0';
        const char* sp = strchr(line, ' ');
        if (sp) status = atoi(sp + 1);
    }
    while (client.available()) {
        size_t len = client.readBytesUntil('\n', line, sizeof(line) - 1);
        line[len] = '\0';
        if (len <= 1) break;
        if (strncasecmp(line, "Retry-After:", 12) == 0) {
            const char* value = line + 12;
            while (*value == ' ' || *value == '\t') value++;
            long seconds = strtol(value, nullptr, 10);
            if (seconds > 0 && seconds <= 300) retryAfterSeconds = (uint16_t)seconds;
        }
    }
    char body[384] = {};
    size_t bodyLen = 0;
    deadline = millis() + 5000;
    while ((client.connected() || client.available()) && bodyLen < sizeof(body) - 1 && millis() < deadline) {
        if (client.available()) body[bodyLen++] = (char)client.read(); else delay(1);
    }
    client.stop();
    lastHttpStatus = status;
    bool ok = status >= 200 && status < 300;
    if (bodyLen) {
        JsonDocument doc;
        if (deserializeJson(doc, body, bodyLen) == DeserializationError::Ok) {
            if (ok && doc["ok"].is<bool>()) ok = doc["ok"].as<bool>();
            if (status == 429 && retryAfterSeconds == 0) {
                long seconds = doc["retry_after"] | 0;
                if (seconds <= 0) seconds = doc["retryAfter"] | 0;
                if (seconds <= 0) seconds = doc["wait_seconds"] | 0;
                if (seconds <= 0) seconds = doc["seconds"] | 0;
                if (seconds > 0 && seconds <= 300) retryAfterSeconds = (uint16_t)seconds;
            }
        }
    }
    if (status == 429 && retryAfterSeconds == 0) retryAfterSeconds = 60;
    if (!ok) {
        if (status == 429) snprintf(lastError, sizeof(lastError), "HTTP 429 WAIT %uS", retryAfterSeconds);
        else snprintf(lastError, sizeof(lastError), status ? "HTTP %d" : "NO RESPONSE", status);
    }
    return ok;
}

void WDGWars::waitForRateLimit(uint16_t seconds, WdgWarsProgressCallback cb) {
    if (seconds == 0) seconds = 60;
    if (seconds > 300) seconds = 300;
    Serial.printf("[WDGWARS] Rate limited; waiting %u seconds\n", (unsigned)seconds);
    for (uint16_t remaining = seconds; remaining > 0; --remaining) {
        if (cb) {
            char status[32];
            snprintf(status, sizeof(status), "RATE LIMIT: WAIT %uS", (unsigned)remaining);
            cb(status, 0, 0);
        }
        delay(1000);
        yield();
    }
}

WdgWarsSyncResult WDGWars::syncFiles(WdgWarsProgressCallback cb) {
    WdgWarsSyncResult result = {};
    if (!hasCredentials()) { strncpy(result.error, "NO WDGWARS API KEY", sizeof(result.error) - 1); return result; }
    if (WiFi.status() != WL_CONNECTED) { strncpy(result.error, "WIFI NOT CONNECTED", sizeof(result.error) - 1); return result; }

    bool recon = NetworkRecon::isRunning();
    if (recon) { NetworkRecon::pause(); NetworkRecon::freeNetworks(); }
    freeUploadedListMemory();
    auto uploadGate = HeapGates::checkGate(WDG_MIN_FREE_FOR_TLS, WDG_MIN_CONTIG_FOR_TLS);
    if (uploadGate.failure != HeapGates::TlsGateFailure::None) {
        WiFiUtils::conditionHeapForTLS();
        uploadGate = HeapGates::checkGate(WDG_MIN_FREE_FOR_TLS, WDG_MIN_CONTIG_FOR_TLS);
        if (!HeapGates::canMeet(uploadGate, lastError, sizeof(lastError))) {
            snprintf(result.error, sizeof(result.error), "%s", lastError);
            Mood::setStatusMessage("HEAP TIGHT - TRY OINK");
            if (recon) NetworkRecon::resume();
            return result;
        }
    }

    // Four files per pass keeps this frame small; rerunning sync continues with
    // the next four because successful files are tracked on SD.
    static constexpr uint8_t MAX_BATCH = 4;
    struct Pending { char path[80]; bool uploaded; } pending[MAX_BATCH] = {};
    uint8_t count = 0;
    loadUploadedList();
    const char* wardrivingDir = SDLayout::wardrivingReadDir();
    File dir = SD.open(wardrivingDir);
    if (dir && dir.isDirectory()) {
        File file = dir.openNextFile();
        while (file && count < MAX_BATCH) {
            const char* name = baseName(file.name());
            if (!file.isDirectory() && wigleCsv(name)) {
                if (isUploaded(name)) result.skipped++;
                else {
                    snprintf(pending[count].path, sizeof(pending[count].path), "%s/%s", wardrivingDir, name);
                    count++;
                }
            }
            file.close(); file = dir.openNextFile(); yield();
        }
        dir.close();
    }
    if (count == 0 && result.skipped == 0) {
        strncpy(result.error, "NO WIGLE CSV FILES", sizeof(result.error) - 1);
        if (recon) NetworkRecon::resume();
        return result;
    }
    freeUploadedListMemory();
    uint16_t pacingSeconds = 0;
    for (uint8_t i = 0; i < count; ++i) {
        if (pacingSeconds > 0 && i > 0) waitForRateLimit(pacingSeconds, cb);
        if (cb) cb("UPLOADING TO WDGWARS", i + 1, count);
        uploadGate = HeapGates::checkGate(WDG_MIN_FREE_FOR_TLS, WDG_MIN_CONTIG_FOR_TLS);
        if (!HeapGates::canMeet(uploadGate, lastError, sizeof(lastError))) {
            result.failed += count - i;
            break;
        }
        bool uploaded = uploadSingleFile(pending[i].path);
        if (!uploaded && lastHttpStatus == 0 && strcmp(lastError, "DNS LOOKUP FAILED") != 0 &&
            strncmp(lastError, "TLS -1 ", 7) == 0) {
            if (cb) cb("TLS RETRY IN 2S", 0, 0);
            HeapGates::waitForLwipCleanup();
            delay(2000);
            yield();
            uploadGate = HeapGates::checkGate(WDG_MIN_FREE_FOR_TLS, WDG_MIN_CONTIG_FOR_TLS);
            if (WiFi.status() == WL_CONNECTED &&
                HeapGates::canMeet(uploadGate, lastError, sizeof(lastError))) {
                uploaded = uploadSingleFile(pending[i].path);  // one transient-connect retry
            }
        }
        if (!uploaded && lastHttpStatus == 429) {
            uint16_t serverWait = retryAfterSeconds ? retryAfterSeconds : 60;
            waitForRateLimit(serverWait, cb);
            uploadGate = HeapGates::checkGate(WDG_MIN_FREE_FOR_TLS, WDG_MIN_CONTIG_FOR_TLS);
            if (HeapGates::canMeet(uploadGate, lastError, sizeof(lastError))) {
                uploaded = uploadSingleFile(pending[i].path);  // one automatic retry
            }
            pacingSeconds = serverWait;  // pace the remainder of this batch
        }
        if (uploaded) {
            pending[i].uploaded = true;
            result.uploaded++;
        } else {
            result.failed++;
            // A connection-level failure applies to the endpoint/session, not
            // this CSV. Retrying every file only fragments heap further.
            if (strncmp(lastError, "TLS ", 4) == 0) break;
        }
        HeapGates::waitForLwipCleanup();
        yield();
    }
    loadUploadedList();
    for (uint8_t i = 0; i < count; ++i) if (pending[i].uploaded) markAsUploaded(pending[i].path);
    saveUploadedList();
    result.success = result.failed == 0;
    if (result.failed) strncpy(result.error, lastError, sizeof(result.error) - 1);
    SDLog::log("WDGWARS", "Sync: uploaded=%u failed=%u skipped=%u", result.uploaded, result.failed, result.skipped);
    if (recon) NetworkRecon::resume();
    return result;
}
