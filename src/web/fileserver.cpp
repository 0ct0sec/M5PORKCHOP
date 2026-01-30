// WiFi File Server implementation

#include "fileserver.h"
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <SD.h>
#include <ESPmDNS.h>
#include <pgmspace.h>
#include <ctype.h>
#include <string.h>
#include <vector>
#include <atomic>
#include "../core/wifi_utils.h"
#include "../core/heap_policy.h"
#include "../core/xp.h"
#include "../ui/swine_stats.h"
#include "../core/sd_layout.h"
#include "wigle.h"

// Static members
WebServer* FileServer::server = nullptr;
FileServerState FileServer::state = FileServerState::IDLE;
char FileServer::statusMessage[64] = "Ready";
char FileServer::targetSSID[64] = "";
char FileServer::targetPassword[64] = "";
uint32_t FileServer::connectStartTime = 0;
uint32_t FileServer::lastReconnectCheck = 0;
uint64_t FileServer::sessionRxBytes = 0;
uint64_t FileServer::sessionTxBytes = 0;
uint32_t FileServer::sessionUploadCount = 0;
uint32_t FileServer::sessionDownloadCount = 0;

// File upload state (needs to be declared early for stop() to access it)
static File uploadFile;
static char uploadDirBuf[128] = "";   // FIX: Fixed buffer instead of String to avoid heap fragmentation
static std::atomic<bool> uploadActive{false};  // FIX: Atomic for cross-context synchronization
static std::atomic<uint32_t> uploadLastProgress{0};  // FIX: Atomic - accessed from callback and update loop
static std::atomic<bool> uploadRejected{false};  // FIX: Atomic for cross-context synchronization
static std::atomic<bool> listActive{false};  // FIX: Atomic for cross-context synchronization
static std::atomic<uint32_t> listStartTime{0};  // FIX: Atomic - accessed from callback and update loop
static char uploadPathBuf[256] = "";  // FIX: Fixed buffer instead of String to avoid heap fragmentation

// XP award tracking (browser-less, device-side)
static const char* XP_WPA_AWARDED_FILE = nullptr;
static const char* XP_WIGLE_AWARDED_FILE = nullptr;
static const char* WPA_SENT_FILE = nullptr;
static const char* WIGLE_UPLOADED_FILE = nullptr;
static const uint16_t XP_WPA_PER = 15;
static const uint16_t XP_WIGLE_PER = 10;
static const uint16_t XP_SESSION_CAP = 200;
static const size_t MIN_PCAP_BYTES = 300;
static const size_t MIN_WIGLE_BYTES = 200;
static const size_t XP_AWARD_CACHE_MAX = 512;
static uint32_t xpLastScanMs = 0;
static uint32_t xpLastUploadCount = 0;
static uint16_t xpSessionAwarded = 0;
static bool xpScanPending = false;
static bool xpWpaLoaded = false;
static bool xpWigleLoaded = false;
static bool xpWpaCacheComplete = false;
static bool xpWigleCacheComplete = false;
static std::vector<String> xpAwardedWpa;
static std::vector<String> xpAwardedWigle;

static void refreshSdPaths() {
    XP_WPA_AWARDED_FILE = SDLayout::xpAwardedWpaPath();
    XP_WIGLE_AWARDED_FILE = SDLayout::xpAwardedWiglePath();
    WPA_SENT_FILE = SDLayout::wpasecSentPath();
    WIGLE_UPLOADED_FILE = SDLayout::wigleUploadedPath();
}

static void logWiFiStatus(const char* label) {
    // Print WiFi status and mode to aid debugging of server connection issues
    (void)label;
    wifi_mode_t mode = WiFi.getMode();
    wl_status_t status = WiFi.status();
    Serial.printf("[FILESERVER] %s WiFi mode=%d status=%d\n", label, (int)mode, (int)status);
}

static void logRequest(WebServer* srv, const char* label) {
    // FIX: Use const char* to avoid String allocation on every request
    (void)label;
    if (!srv) return;
    const char* method;
    switch (srv->method()) {
        case HTTP_GET: method = "GET"; break;
        case HTTP_POST: method = "POST"; break;
        case HTTP_PUT: method = "PUT"; break;
        case HTTP_DELETE: method = "DELETE"; break;
        default: method = "OTHER"; break;  // Avoid String allocation for rare case
    }
    Serial.printf("[FILESERVER] %s %s\n", method, srv->uri().c_str());
}

static void logHeapStatus(const char* label) {
    // Log current free heap and largest free block for debugging memory usage
    size_t freeHeap = ESP.getFreeHeap();
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    Serial.printf("[FILESERVER] %s heap free=%u largest=%u\n", label ? label : "heap", (unsigned int)freeHeap, (unsigned int)largest);
}

static void logHeapStatusIfLow(const char* label) {
    // If free heap drops below 60k, log details to help trace memory issues
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 60000) {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        Serial.printf("[FILESERVER] %s low heap free=%u largest=%u\n", label ? label : "low heap", (unsigned int)freeHeap, (unsigned int)largest);
    }
}

static const size_t UI_MIN_FREE = 12000;
static const size_t UI_MIN_LARGEST = 8000;

static bool isUiHeapLow(size_t* outFree, size_t* outLargest) {
    size_t freeHeap = ESP.getFreeHeap();
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (outFree) *outFree = freeHeap;
    if (outLargest) *outLargest = largest;
    return (freeHeap < UI_MIN_FREE) || (largest < UI_MIN_LARGEST);
}

static size_t sendProgmemResponse(WebServer* srv, int status, const char* contentType, const char* data) {
    if (!srv || !data) return 0;
    const size_t totalLen = strlen_P(data);
    srv->sendHeader("Connection", "close");
    srv->sendHeader("Cache-Control", "no-store");
    srv->setContentLength(totalLen);
    srv->send(status, contentType, "");

    WiFiClient client = srv->client();
    client.setNoDelay(true);

    const size_t chunkSize = 512;
    size_t offset = 0;
    uint32_t lastProgress = millis();

    while (offset < totalLen && client.connected()) {
        size_t len = totalLen - offset;
        if (len > chunkSize) {
            len = chunkSize;
        }
        size_t sent = client.write_P(data + offset, len);
        if (sent == 0) {
            if (millis() - lastProgress > 2000) {
                break;
            }
            delay(1);
            yield();
            continue;
        }
        offset += sent;
        lastProgress = millis();
        yield();
    }
    client.flush();
    return offset;
}

static size_t sendProgmemResponse(WebServer* srv, const char* contentType, const char* data) {
    return sendProgmemResponse(srv, 200, contentType, data);
}

static const char LOW_HEAP_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>LOW HEAP</title>
</head>
<body style="background:#2E1A47;color:#C8B2FF;font-family:Courier New,monospace;padding:16px">
    low heap. run oink, then reload.
</body>
</html>
)rawliteral";

static bool isTransferBusy() {
    return uploadActive.load();
}

static void sendBusyResponse(WebServer* srv) {
    srv->sendHeader("Connection", "close");
    srv->send(503, "text/plain", "BUSY");
}

static void resetUploadState(bool removePartial) {
    if (uploadFile) {
        uploadFile.close();
    }
    if (removePartial && uploadPathBuf[0] != '\0') {
        SD.remove(uploadPathBuf);
    }
    uploadActive.store(false);
    uploadRejected.store(false);
    uploadPathBuf[0] = '\0';
    uploadDirBuf[0] = '\0';
}

static bool listContains(const std::vector<String>& list, const String& value) {
    for (const auto& entry : list) {
        if (entry == value) return true;
    }
    return false;
}

// FIX: Rewritten to avoid heap allocations in hot path.
// Uses char* comparisons instead of creating temporary String objects.
static String mapUiPathToFs(const String& path) {
    if (path.isEmpty()) return path;
    if (SDLayout::usingNewLayout()) return path;
    
    const char* root = SDLayout::newRoot();
    const size_t rootLen = root ? strlen(root) : 0;
    if (rootLen == 0) return path;
    
    const char* p = path.c_str();
    const size_t pathLen = path.length();
    
    // Check if path starts with root prefix
    if (pathLen < rootLen || strncmp(p, root, rootLen) != 0) {
        return path;
    }
    
    // Path starts with root - check what follows
    const char* suffix = p + rootLen;
    const size_t suffixLen = pathLen - rootLen;
    
    // Direct virtual folder mappings (e.g., /porkchop/config -> /)
    static const char* const VIRTUAL_FOLDERS[] = {
        "/config", "/wpa-sec", "/wigle", "/xp", "/misc", "/diagnostics", "/meta"
    };
    static const size_t VIRTUAL_FOLDER_COUNT = sizeof(VIRTUAL_FOLDERS) / sizeof(VIRTUAL_FOLDERS[0]);
    
    for (size_t i = 0; i < VIRTUAL_FOLDER_COUNT; i++) {
        const size_t folderLen = strlen(VIRTUAL_FOLDERS[i]);
        if (suffixLen == folderLen && strncmp(suffix, VIRTUAL_FOLDERS[i], folderLen) == 0) {
            return "/";
        }
        // Check if path is under this virtual folder (e.g., /porkchop/config/file.txt)
        if (suffixLen > folderLen && 
            strncmp(suffix, VIRTUAL_FOLDERS[i], folderLen) == 0 && 
            suffix[folderLen] == '/') {
            // Return the part after the virtual folder prefix
            return String(suffix + folderLen);  // Only one String allocation for result
        }
    }
    
    // Just root prefix - strip it
    if (suffixLen == 0) return "/";
    return String(suffix);  // Only one String allocation for result
}

static const char* basenameFromPath(const char* path) {
    if (!path) return "";
    const char* last = strrchr(path, '/');
    if (!last) return path;
    if (*(last + 1) == '\0') return path;
    return last + 1;
}

static bool isSameOrSubPath(const String& parent, const String& child) {
    if (parent.isEmpty() || child.isEmpty()) return false;
    String p = parent;
    String c = child;
    if (p.length() > 1 && p.endsWith("/")) p.remove(p.length() - 1);
    if (c.length() > 1 && c.endsWith("/")) c.remove(c.length() - 1);
    if (c == p) return true;
    if (p == "/") return false;
    if (!c.startsWith(p)) return false;
    return (c.length() > p.length() && c.charAt(p.length()) == '/');
}

static void loadAwardedList(const char* path, std::vector<String>& out, bool& loaded, bool& complete) {
    if (loaded) return;
    if (!path || !path[0]) {
        out.clear();
        complete = true;
        loaded = true;
        return;
    }
    out.clear();
    out.reserve(XP_AWARD_CACHE_MAX);  // FIX: Pre-allocate to avoid realloc fragmentation
    complete = true;
    File f = SD.open(path, FILE_READ);
    if (f) {
        String line;
        line.reserve(128);  // Pre-allocate to reduce heap fragmentation
        while (f.available()) {
            yield();  // FIX: Prevent WDT on large files
            line = f.readStringUntil('\n');  // Reuses existing capacity
            line.trim();
            if (line.length()) {
                if (out.size() < XP_AWARD_CACHE_MAX) {
                    out.push_back(line);
                } else {
                    complete = false;
                    break;
                }
            }
        }
        f.close();
    }
    loaded = true;
}

static bool appendAwarded(const char* path, std::vector<String>& out, const String& value) {
    if (!path || !path[0]) return false;
    if (listContains(out, value)) return false;
    if (out.size() >= XP_AWARD_CACHE_MAX) return false;  // FIX: Cap vector size
    File f = SD.open(path, FILE_APPEND);
    if (!f) return false;
    f.println(value);
    f.close();
    out.push_back(value);
    return true;
}

static bool fileHasLine(const char* path, const String& value) {
    if (!path || !path[0]) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    String line;
    line.reserve(128);  // Pre-allocate to reduce heap fragmentation
    while (f.available()) {
        yield();  // FIX: Prevent WDT on large files
        line = f.readStringUntil('\n');  // Reuses existing capacity
        line.trim();
        if (!line.length()) continue;
        if (line == value) {
            f.close();
            return true;
        }
    }
    f.close();
    return false;
}

static bool isAwarded(const char* path,
                      const String& value,
                      std::vector<String>& cache,
                      bool& loaded,
                      bool& complete) {
    loadAwardedList(path, cache, loaded, complete);
    if (!path || !path[0]) return false;
    if (listContains(cache, value)) return true;
    if (complete) return false;
    return fileHasLine(path, value);
}

static String normalizeHexToken(const String& input, size_t maxLen) {
    String out;
    out.reserve(maxLen);
    for (size_t i = 0; i < input.length(); i++) {
        const char c = input[i];
        if (isxdigit(static_cast<unsigned char>(c))) {
            out += (char)toupper(static_cast<unsigned char>(c));
            if (out.length() >= maxLen) break;
        }
    }
    return out;
}

// FIX: Accept const char* directly to avoid implicit String construction from char[]
static bool pcapLooksValid(const char* path) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    const size_t size = f.size();
    if (size < MIN_PCAP_BYTES) {
        f.close();
        return false;
    }
    uint8_t hdr[4] = {0};
    if (f.read(hdr, sizeof(hdr)) != sizeof(hdr)) {
        f.close();
        return false;
    }
    f.close();
    const uint8_t pcapMagic[][4] = {
        {0xd4, 0xc3, 0xb2, 0xa1},
        {0xa1, 0xb2, 0xc3, 0xd4},
        {0x4d, 0x3c, 0xb2, 0xa1},
        {0xa1, 0xb2, 0x3c, 0x4d},
        {0x0a, 0x0d, 0x0d, 0x0a}  // pcapng
    };
    for (size_t i = 0; i < sizeof(pcapMagic) / sizeof(pcapMagic[0]); i++) {
        if (memcmp(hdr, pcapMagic[i], 4) == 0) {
            return true;
        }
    }
    return false;
}

// Overload for String& - delegates to const char* version
static bool pcapLooksValid(const String& path) {
    return pcapLooksValid(path.c_str());
}

// FIX: Accept const char* directly to avoid implicit String construction from char[]
static bool wigleLooksValid(const char* path) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    const size_t size = f.size();
    if (size < MIN_WIGLE_BYTES) {
        f.close();
        return false;
    }
    const int maxLines = 5;
    bool valid = false;
    String line;
    line.reserve(256);  // Pre-allocate for header lines which can be long
    for (int i = 0; i < maxLines && f.available(); i++) {
        line = f.readStringUntil('\n');  // Reuses existing capacity
        line.trim();
        if (!line.length()) continue;
        if (line.startsWith("\xEF\xBB\xBF")) {
            line = line.substring(3);
            line.trim();
        }
        line.toLowerCase();  // FIX: In-place, no copy needed
        if (line.indexOf("wigle") >= 0) { valid = true; break; }
        if (line.startsWith("mac,") || line.startsWith("bssid,")) { valid = true; break; }
    }
    f.close();
    if (valid) return true;
    if (size >= (MIN_WIGLE_BYTES * 4)) return true;
    return false;
}

// Overload for String& - delegates to const char* version
static bool wigleLooksValid(const String& path) {
    return wigleLooksValid(path.c_str());
}

static bool awardXpEntry(const char* src, uint16_t per, const char* awardFile, std::vector<String>& awardList, const String& key) {
    if (xpSessionAwarded + per > XP_SESSION_CAP) {
        return false;
    }
    if (!appendAwarded(awardFile, awardList, key)) {
        return false;
    }
    XP::addXP(per);
    xpSessionAwarded += per;
    Serial.printf("[FILESERVER] XP AWARD %s +%u\n", src, (unsigned int)per);
    return true;
}

static void scanXpAwards() {
    if (uploadActive.load()) {
        xpScanPending = true;
        return;
    }
    if (xpSessionAwarded >= XP_SESSION_CAP) return;
    if (!XP_WPA_AWARDED_FILE || !XP_WIGLE_AWARDED_FILE || !WPA_SENT_FILE || !WIGLE_UPLOADED_FILE) {
        refreshSdPaths();
    }
    if (!XP_WPA_AWARDED_FILE || !XP_WIGLE_AWARDED_FILE || !WPA_SENT_FILE || !WIGLE_UPLOADED_FILE) {
        return;
    }

    loadAwardedList(XP_WPA_AWARDED_FILE, xpAwardedWpa, xpWpaLoaded, xpWpaCacheComplete);
    loadAwardedList(XP_WIGLE_AWARDED_FILE, xpAwardedWigle, xpWigleLoaded, xpWigleCacheComplete);

    // WPA-SEC awards
    // FIX: Reuse String objects to reduce heap fragmentation
    File wpaFile = SD.open(WPA_SENT_FILE, FILE_READ);
    if (!wpaFile) {
        wpaFile = SD.open(SDLayout::wpasecUploadedPath(), FILE_READ);
    }
    if (wpaFile) {
        String line;
        line.reserve(64);  // Pre-allocate for typical BSSID lines
        String bssid;
        bssid.reserve(16);
        char pcapPathBuf[128];  // FIX: Stack buffer for path building
        const char* hsDir = SDLayout::handshakesDir();
        
        while (wpaFile.available() && xpSessionAwarded < XP_SESSION_CAP) {
            yield();  // FIX: Prevent WDT on large files
            line = wpaFile.readStringUntil('\n');
            line.trim();
            if (!line.length()) continue;
            bssid = normalizeHexToken(line, 12);
            if (bssid.length() < 12) continue;
            if (isAwarded(XP_WPA_AWARDED_FILE, bssid, xpAwardedWpa, xpWpaLoaded, xpWpaCacheComplete)) continue;
            snprintf(pcapPathBuf, sizeof(pcapPathBuf), "%s/%s.pcap", hsDir, bssid.c_str());
            if (!pcapLooksValid(pcapPathBuf)) continue;
            awardXpEntry("WPA", XP_WPA_PER, XP_WPA_AWARDED_FILE, xpAwardedWpa, bssid);
        }
        wpaFile.close();
    }

    // WiGLE awards
    // FIX: Reuse String objects to reduce heap fragmentation
    File wigleFile = SD.open(WIGLE_UPLOADED_FILE, FILE_READ);
    if (wigleFile) {
        String line;
        line.reserve(128);  // Pre-allocate for path lines
        String path;
        path.reserve(128);
        char pathBuf[160];  // FIX: Stack buffer for path building
        const char* wdDir = SDLayout::wardrivingDir();
        
        while (wigleFile.available() && xpSessionAwarded < XP_SESSION_CAP) {
            yield();  // FIX: Prevent WDT on large files
            line = wigleFile.readStringUntil('\n');
            line.trim();
            if (!line.length()) continue;
            
            // FIX: Build path in stack buffer when needed, else reuse line
            if (line.charAt(0) != '/') {
                snprintf(pathBuf, sizeof(pathBuf), "%s/%s", wdDir, line.c_str());
                path = pathBuf;
            } else {
                path = line;
            }
            
            // FIX: Check extension without creating lowercase copy
            size_t pathLen = path.length();
            if (pathLen < 10) continue;  // ".wigle.csv" is 10 chars
            const char* ext = path.c_str() + pathLen - 10;
            if (strcasecmp(ext, ".wigle.csv") != 0) continue;
            
            if (isAwarded(XP_WIGLE_AWARDED_FILE, path, xpAwardedWigle, xpWigleLoaded, xpWigleCacheComplete)) continue;
            if (!wigleLooksValid(path)) continue;
            awardXpEntry("WIGLE", XP_WIGLE_PER, XP_WIGLE_AWARDED_FILE, xpAwardedWigle, path);
        }
        wigleFile.close();
    }
}

static void appendJsonEscaped(String& out, const char* in) {
    // FIX: Pre-reserve space to avoid repeated reallocs.
    // Worst case: every char needs escaping (2x size).
    const size_t inLen = strlen(in);
    out.reserve(out.length() + inLen + (inLen / 4));  // +25% for escapes
    
    // FIX: Batch small runs of safe chars to reduce concat calls
    const char* runStart = in;
    while (*in) {
        char c = *in;
        if (c == '\"' || c == '\\' || static_cast<uint8_t>(c) < 0x20) {
            // Flush any pending safe chars first
            if (in > runStart) {
                out.concat(runStart, in - runStart);
            }
            // Handle special char
            if (c == '\"') {
                out += "\\\"";
            } else if (c == '\\') {
                out += "\\\\";
            } else {
                out += ' ';
            }
            in++;
            runStart = in;
        } else {
            in++;
        }
    }
    // Flush remaining safe chars
    if (in > runStart) {
        out.concat(runStart, in - runStart);
    }
}

static const char* pickMoodName(uint8_t flags, bool debuff) {
    if (!flags) return "N0N3";
    for (uint8_t i = 0; i < 8; i++) {
        uint8_t mask = (1 << i);
        if (flags & mask) {
            if (debuff) {
                return SwineStats::getDebuffName((PorkDebuff)mask);
            }
            return SwineStats::getBuffName((PorkBuff)mask);
        }
    }
    return "N0N3";
}

// FIX: Helper to write JSON-escaped string to a char buffer
// Returns number of chars written (not including null terminator)
static size_t writeJsonEscaped(char* buf, size_t bufSize, const char* in) {
    if (!buf || bufSize == 0 || !in) return 0;
    size_t written = 0;
    while (*in && written < bufSize - 1) {
        char c = *in;
        if (c == '\"' || c == '\\') {
            if (written + 2 >= bufSize) break;
            buf[written++] = '\\';
            buf[written++] = c;
        } else if (static_cast<uint8_t>(c) < 0x20) {
            buf[written++] = ' ';  // Replace control chars with space
        } else {
            buf[written++] = c;
        }
        in++;
    }
    buf[written] = '\0';
    return written;
}

// FIX: Static buffer version to avoid heap allocation per call
static char swineSummaryBuf[512];

static const char* buildSwineSummaryJson() {
    const uint8_t level = XP::getLevel();
    const char* title = XP::getDisplayTitle();
    const char* className = XP::getClassName();
    const uint8_t progress = XP::getProgress();
    const uint32_t totalXP = XP::getTotalXP();
    const uint32_t xpToNext = XP::getXPToNextLevel();
    const uint8_t achUnlocked = XP::getUnlockedCount();
    const uint8_t achTotal = XP::getAchievementCount();

    const WiGLE::WigleUserStats stats = WiGLE::getUserStats();

    const BuffState buffs = SwineStats::calculateBuffs();
    const char* moodType = "NONE";
    const char* moodName = "N0N3";
    if (buffs.buffs) {
        moodType = "BUFF";
        moodName = pickMoodName(buffs.buffs, false);
    } else if (buffs.debuffs) {
        moodType = "DEBUFF";
        moodName = pickMoodName(buffs.debuffs, true);
    }

    // Escape strings into temp buffers
    char titleEsc[64], classEsc[64], moodTypeEsc[16], moodNameEsc[32];
    writeJsonEscaped(titleEsc, sizeof(titleEsc), title);
    writeJsonEscaped(classEsc, sizeof(classEsc), className);
    writeJsonEscaped(moodTypeEsc, sizeof(moodTypeEsc), moodType);
    writeJsonEscaped(moodNameEsc, sizeof(moodNameEsc), moodName);

    snprintf(swineSummaryBuf, sizeof(swineSummaryBuf),
        "{\"level\":%u,\"title\":\"%s\",\"titleOverride\":%u,\"className\":\"%s\","
        "\"xpTotal\":%lu,\"xpProgress\":%u,\"xpToNext\":%lu,"
        "\"achUnlocked\":%u,\"achTotal\":%u,"
        "\"wigleValid\":%s,\"wigleRank\":%lld,\"wigleWifi\":%llu,\"wigleCell\":%llu,\"wigleBt\":%llu,"
        "\"moodType\":\"%s\",\"mood\":\"%s\"}",
        (unsigned)level, titleEsc, (unsigned)XP::getTitleOverride(), classEsc,
        (unsigned long)totalXP, (unsigned)progress, (unsigned long)xpToNext,
        (unsigned)achUnlocked, (unsigned)achTotal,
        stats.valid ? "true" : "false",
        (long long)stats.rank, (unsigned long long)stats.wifi,
        (unsigned long long)stats.cell, (unsigned long long)stats.bt,
        moodTypeEsc, moodNameEsc);

    return swineSummaryBuf;
}

// Black & white HTML interface - Midnight Commander style dual-pane
static const char HTML_STYLE[] PROGMEM = R"rawliteral(
/* ======================================================================
   PC64.EXE embedded file server UI
   Midnight Commander layout + Electric Blue palette
   Oldschool terminal aesthetic - spartan and effective
   ====================================================================== */

:root{
  /* --- Electric Blue palette --- */
  --dr-bg: #0D1117;
  --dr-fg: #E6EDF3;
  --dr-current: #161B22;
  --dr-comment: #7D8590;
  --dr-cyan: #00FFFF;
  --dr-green: #3FB950;
  --dr-orange: #F0883E;
  --dr-pink: #00BFFF;
  --dr-purple: #00BFFF;
  --dr-red: #F85149;
  --dr-yellow: #D29922;

  /* --- Semantic tokens (MC-ish) --- */
  --bg: var(--dr-bg);
  --fg: var(--dr-fg);
  --dim: rgba(230,237,243,.60);

  /* Borders: same as dim text, NOT accent colored */
  --border-soft: rgba(230,237,243,.20);
  --col-sep: rgba(230,237,243,.25);

  --panel-bg: #0D1117;

  /* MC header bars - electric blue */
  --title-inactive-bg: var(--dr-current);
  --title-inactive-fg: var(--fg);
  --title-active-bg: #0969DA;
  --title-active-fg: #FFFFFF;

  /* Cursor line - electric blue */
  --focus-bg: #00BFFF;
  --focus-fg: #000000;

  --mark-fg: var(--dr-green);
  --warn-fg: var(--dr-orange);
  --danger-fg: var(--dr-red);

  /* Function key hotkey */
  --key-bg: #0969DA;
  --key-fg: #FFFFFF;

  --shadow: rgba(0,0,0,.50);

  --frame-gap: 4px;
  --pad-x: 8px;
  --fs: 13px;
}

*{ box-sizing:border-box; margin:0; padding:0; }
html, body{ height:100%; }
body.mc{
  background: var(--bg);
  color: var(--fg);
  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace;
  font-size: var(--fs);
  line-height: 1.25;
  display:flex;
  flex-direction:column;
  overflow:hidden;
  font-variant-ligatures: none;
}

/* ----------------------------------------------------------------------
   Top bars (MC-like)
   ---------------------------------------------------------------------- */
.header{
  padding: 5px var(--pad-x);
  display:flex;
  justify-content:space-between;
  align-items:center;
  flex-shrink:0;
  background: var(--title-active-bg);
  color: var(--title-active-fg);
  border-bottom: 1px solid var(--border);
}
.header h1{
  font-size: 1em;
  font-weight: normal;
  letter-spacing: .6px;
}
.sd-info{
  font-size: .9em;
  opacity: .90;
  white-space: nowrap;
}

.swine-strip{
  padding: 4px var(--pad-x);
  background: var(--panel-bg);
  border-bottom: 1px solid var(--border);
  font-size: .9em;
  flex-shrink:0;
}
.swine-line{
  white-space:nowrap;
  overflow:hidden;
  text-overflow:ellipsis;
}
.swine-line + .swine-line{ opacity:.88; }

/* ----------------------------------------------------------------------
   Main workspace frame
   ---------------------------------------------------------------------- */
.main{
  flex:1;
  display:grid;
  grid-template-rows: minmax(0, 1fr) minmax(0, .72fr);
  min-height:0;
  background: var(--bg);
}

/* Outer border like MC */
.panes{
  display:flex;
  min-height:0;
  overflow:hidden;

  margin: var(--frame-gap) var(--frame-gap) 0 var(--frame-gap);
  border: 1px solid var(--border);
  background: var(--panel-bg);
  box-shadow: inset 0 0 0 1px rgba(0,0,0,.18);
}

/* Two panes inside the workspace border */
.pane{
  flex:1;
  display:flex;
  flex-direction:column;
  min-height:0;
  overflow:hidden;
  background: var(--panel-bg);
}
.pane + .pane{
  border-left: 1px solid var(--border);
}

/* Pane title bar (MC-like with box-drawing decorations) */
.pane-header{
  flex-shrink:0;
  height: 1.4em;
  background: var(--panel-bg);
  display:flex;
  align-items:center;
  font-size: .9em;
  overflow:hidden;
}
.pane-decor-left{
  color: var(--border);
  white-space:nowrap;
  overflow:hidden;
  text-overflow:clip;
  flex: 1;
  text-align: right;
  padding-left: 2px;
}
.pane-decor-right{
  color: var(--border);
  white-space:nowrap;
  overflow:hidden;
  text-overflow:clip;
  flex: 1;
  text-align: left;
  padding-right: 2px;
}
.pane-path{
  flex-shrink: 0;
  white-space:nowrap;
  overflow:hidden;
  text-overflow:ellipsis;
  max-width: 60%;
  padding: 0 2px;
  color: var(--fg);
  background: var(--panel-bg);
}
.pane.active .pane-decor-left,
.pane.active .pane-decor-right{
  color: var(--dr-cyan);
}

/* Column headers row - MC style with vertical separators */
.col-header{
  display:grid;
  grid-template-columns: 3ch 1fr 8ch 12ch;
  gap: 0;
  padding: 2px 4px;
  background: var(--panel-bg);
  font-size: 1em;
  color: var(--dim);
  text-transform: uppercase;
  letter-spacing: .3px;
  border-bottom: 1px solid var(--border);
}
.col-header > div{
  overflow:hidden;
  text-overflow:ellipsis;
  white-space:nowrap;
  padding: 0 4px;
  border-right: 1px solid var(--col-sep);
}
.col-header > div:last-child{
  border-right: none;
}
.col-header .col-size{
  text-align:right;
  padding-right: 4px;
}
.col-header .col-time{
  text-align:right;
  padding-right: 4px;
}

.file-list{
  flex:1;
  overflow-y:auto;
  overflow-x:hidden;
  background: var(--panel-bg);
}

/* Pane footer with path + disk usage (MC-style with box-drawing) */
.pane-footer{
  flex-shrink:0;
  height: 1.4em;
  background: var(--panel-bg);
  display:flex;
  align-items:center;
  font-size: .85em;
  overflow:hidden;
}
.pane-footer-decor-left{
  color: var(--border);
  white-space:nowrap;
  overflow:hidden;
  text-overflow:clip;
  flex: 1;
  text-align: right;
  padding-left: 2px;
}
.pane-footer-decor-right{
  color: var(--border);
  white-space:nowrap;
  overflow:hidden;
  text-overflow:clip;
  flex: 1;
  text-align: left;
  padding-right: 2px;
}
.pane-footer-disk{
  white-space:nowrap;
  flex-shrink:0;
}
.pane.active .pane-footer-decor-left,
.pane.active .pane-footer-decor-right{
  color: var(--dr-cyan);
}

/* Hide scrollbars (embedded vibe) */
.file-list, .queue-list{
  scrollbar-width: none;
  -ms-overflow-style: none;
}
.file-list::-webkit-scrollbar,
.queue-list::-webkit-scrollbar{ display:none; }

/* ----------------------------------------------------------------------
   File rows (MC-like density with 4 columns + vertical separators)
   ---------------------------------------------------------------------- */
.file-item{
  display:grid;
  grid-template-columns: 3ch 1fr 8ch 12ch;
  align-items:center;
  gap: 0;
  padding: 2px 4px;
  cursor:pointer;
  background: transparent;
  min-width:0;
}

.file-prefix{
  font-weight: normal;
  white-space:nowrap;
  padding: 0 4px;
  border-right: 1px solid var(--col-sep);
}
.file-prefix.dir{
  color: var(--dr-cyan);
}
.file-prefix.exec{
  color: var(--dr-green);
}
.file-prefix.file{
  color: var(--dim);
}
.file-name{
  overflow:hidden;
  text-overflow:ellipsis;
  white-space:nowrap;
  min-width:0;
  padding: 0 4px;
  border-right: 1px solid var(--col-sep);
}
.file-name.dir{
  color: var(--dr-cyan);
}
.file-name.exec{
  color: var(--dr-green);
}
.file-size{
  color: var(--dim);
  text-align:right;
  font-size: .95em;
  font-variant-numeric: tabular-nums;
  white-space:nowrap;
  padding: 0 4px;
  border-right: 1px solid var(--col-sep);
}
.file-time{
  color: var(--dim);
  text-align:right;
  font-size: .9em;
  font-variant-numeric: tabular-nums;
  white-space:nowrap;
  padding: 0 4px;
}

/* Hover resembles MC "current line" but subtle */
.file-item:hover{
  background: rgba(68,71,90,.38);
}

/* Marked/selected items */
.file-item.selected{
  background: rgba(80,250,123,.12);
}
.file-item.selected .file-prefix,
.file-item.selected .file-name{
  color: var(--mark-fg);
}

/* Cursor line: inverse (Dracula MC uses a vivid highlight) */
.file-item.focused{
  background: var(--focus-bg);
  color: var(--focus-fg);
}
.file-item.focused .file-prefix,
.file-item.focused .file-name,
.file-item.focused .file-size,
.file-item.focused .file-time{
  color: var(--focus-fg);
}

/* Cursor + marked: marker still "marked" */
.file-item.selected.focused{
  background: var(--focus-bg);
}
.file-item.selected.focused .file-prefix{
  color: var(--mark-fg);
}

/* ----------------------------------------------------------------------
   Ops panels (bottom half) - styled like MC panels
   ---------------------------------------------------------------------- */
.ops{
  display:grid;
  grid-template-columns: 1fr 1fr;
  gap: 0;
  margin: var(--frame-gap);
  border: 1px solid var(--border);
  background: var(--panel-bg2);
  min-height:0;
  overflow:hidden;
}
.ops-panel{
  padding: 0;
  overflow:hidden;
  display:flex;
  flex-direction:column;
  min-height:0;
}
.ops-panel + .ops-panel{
  border-left: 1px solid var(--border);
}
.ops-block{
  display:flex;
  flex-direction:column;
  min-height:0;
  height:100%;
}
.ops-header{
  display:flex;
  align-items:center;
  height: 1.4em;
  background: var(--panel-bg);
  font-size: .9em;
  overflow:hidden;
}
.ops-decor-left{
  color: var(--border);
  white-space:nowrap;
  overflow:hidden;
  text-overflow:clip;
  flex: 1;
  text-align: right;
  padding-left: 2px;
}
.ops-decor-right{
  color: var(--border);
  white-space:nowrap;
  overflow:hidden;
  text-overflow:clip;
  flex: 1;
  text-align: left;
  padding-right: 2px;
}
.ops-title-wrap{
  flex-shrink: 0;
  display:flex;
  align-items:center;
  gap: 8px;
  background: var(--panel-bg);
  padding: 0 2px;
}
.ops-title{
  text-transform: uppercase;
  letter-spacing: .5px;
  color: var(--fg);
  white-space:nowrap;
}
.ops-meta{
  white-space:nowrap;
  overflow:hidden;
  text-overflow:ellipsis;
  color: var(--dim);
}
.ops-actions{
  display:flex;
  gap: 6px;
}

/* Queues are "tables" => fixed columns + separators for perfect alignment */
.queue-head, .queue-row{
  display:grid;
  gap: 0; /* separators act as gaps */
  padding: 2px 8px;
  font-size: .9em;
  align-items:center;
  font-variant-numeric: tabular-nums;
  min-width:0;
}
.queue-head{
  background: var(--panel-bg2);
  border-bottom: 1px solid var(--border);
  text-transform: uppercase;
  letter-spacing: .4px;
  color: var(--fg);
}
.queue-head.wpa, .queue-row.wpa{
  grid-template-columns:
    minmax(18ch, 1.55fr)
    minmax(10ch, 1.00fr)
    minmax(10ch, 1.00fr)
    10ch;
}
.queue-head.wigle, .queue-row.wigle{
  grid-template-columns:
    minmax(20ch, 1.60fr)
    7ch
    10ch;
}

.queue-head > div, .queue-row > div{
  overflow:hidden;
  text-overflow:ellipsis;
  white-space:nowrap;
  min-width:0;
}
.queue-head > div:not(:first-child),
.queue-row > div:not(:first-child){
  padding-left: 1ch;
}
.queue-list{
  flex:1;
  min-height:0;
  overflow-y:auto;
  background: var(--panel-bg);
}

.queue-dim{ opacity:.88; color: var(--dim); }
.queue-row .queue-dim{ color: var(--dim); }

/* Column-specific alignment */
.queue-head.wigle > div:nth-child(2),
.queue-row.wigle > div:nth-child(2){
  text-align:right;
  padding-right: 1ch;
}
.queue-head .queue-status{ text-align:center; }
.queue-row .queue-status{
  text-align:left;
  font-weight: 600;
  letter-spacing: .2px;
}
.queue-row.wigle .queue-status{ text-align:left; }

.status-ok{ color: var(--dr-green); }
.status-wait{ color: var(--dr-yellow); }
.status-local{ color: var(--dr-cyan); }
.status-cracked{ color: var(--dr-orange); }

/* ----------------------------------------------------------------------
   Buttons - keep behavior, render like MC-like flat buttons
   ---------------------------------------------------------------------- */
.btn{
  background: transparent;
  color: var(--fg);
  border: 1px solid var(--border-soft);
  padding: 3px 10px;
  cursor:pointer;
  font-family: inherit;
  font-size: .9em;
  letter-spacing: .3px;
}
.btn:hover{ background: rgba(68,71,90,.40); border-color: var(--border); }
.btn:disabled{ opacity:.35; cursor:not-allowed; }
.btn-outline{
  background: transparent;
  color: var(--fg);
  border: 1px solid var(--border);
}
.btn-outline:hover{ background: rgba(68,71,90,.52); }

/* ----------------------------------------------------------------------
   Function key bar + status line
   ---------------------------------------------------------------------- */
.fkey-bar{
  display:flex;
  background: var(--panel-bg);
  border-top: 1px solid var(--border);
  flex-shrink:0;
}
.fkey{
  flex:1;
  padding: 4px 6px;
  text-align:center;
  font-size: .9em;
  border-right: 1px solid var(--border-soft);
  cursor:pointer;
  user-select:none;
  white-space:nowrap;
  overflow:hidden;
  text-overflow:ellipsis;
}
.fkey:last-child{ border-right:none; }
.fkey:hover{ background: rgba(68,71,90,.40); }
.fkey span{
  display:inline-block;
  padding: 0 4px;
  margin-right: 6px;
  background: var(--key-bg);
  color: var(--key-fg);
  border-radius: 0;
}

.status{
  padding: 4px var(--pad-x);
  font-size: .95em;
  background: var(--panel-bg2);
  color: var(--fg);
  border-top: 1px solid var(--border);
  min-height: 22px;
  flex-shrink:0;
  white-space:nowrap;
  overflow:hidden;
  text-overflow:ellipsis;
}

/* Progress bar: keep tiny, MC-like */
.progress-bar{
  height: 4px;
  background: rgba(68,71,90,.55);
  display:none;
}
.progress-bar.active{ display:block; }
.progress-fill{
  height:100%;
  background: var(--dr-green);
  width:0%;
  transition: width .1s linear;
}

/* ----------------------------------------------------------------------
   Dialogs / Modals (MC dialog vibe)
   ---------------------------------------------------------------------- */
.modal{
  display:none;
  position:fixed;
  inset:0;
  background: rgba(0,0,0,.85);
  justify-content:center;
  align-items:center;
  z-index:100;
}
.modal-content{
  background: var(--panel-bg);
  border: 1px solid var(--border);
  box-shadow: 0 14px 40px var(--shadow);
  padding: 14px;
  max-width: 460px;
  width: 92%;
}
.modal-content h3{
  margin: -14px -14px 12px -14px;
  padding: 6px 10px;
  font-weight: normal;
  background: var(--title-active-bg);
  color: var(--title-active-fg);
  border-bottom: 1px solid var(--border);
  letter-spacing: .6px;
}
.modal-body{
  font-size: .95em;
  line-height: 1.5;
  opacity: .95;
}
.modal-tip{
  margin-top: 8px;
  color: var(--dim);
  font-size: .9em;
}
.modal-actions{
  display:flex;
  gap: 10px;
  margin-top: 12px;
}

/* Log console */
.log-console{
  background: var(--panel-bg);
  border: 1px solid var(--border);
  box-shadow: 0 14px 40px var(--shadow);
  padding: 0;
  max-width: 620px;
  width: 92%;
}
.log-console h3{
  margin: 0;
  padding: 6px 10px;
  font-weight: normal;
  background: var(--title-active-bg);
  color: var(--title-active-fg);
  border-bottom: 1px solid var(--border);
}
.log-console pre{
  font-family: inherit;
  font-size: .95em;
  line-height: 1.35;
  margin: 0;
  padding: 10px;
  background: #191A22;
  color: var(--fg);
  min-height: 7em;
  white-space: pre-wrap;
}

/* ----------------------------------------------------------------------
   Embedded editor (mcedit-ish)
   ---------------------------------------------------------------------- */
.editor-content{
  background: var(--panel-bg);
  border: 1px solid var(--border);
  box-shadow: 0 14px 40px var(--shadow);
  padding: 0;
  max-width: 820px;
  width: 94%;
  height: 82vh;
  display:flex;
  flex-direction:column;
  gap: 0;
}
.editor-header{
  background: var(--title-active-bg);
  color: var(--title-active-fg);
  padding: 6px 10px;
  font-size: .95em;
  display:flex;
  justify-content:space-between;
  align-items:center;
  border-bottom: 1px solid var(--border);
}
.editor-title{
  font-weight: normal;
  letter-spacing: .6px;
}
.editor-meta{ opacity:.9; }
.editor-body{
  flex:1;
  display:flex;
  background: #191A22;
}
.editor-textarea{
  width:100%;
  height:100%;
  resize:none;
  background: transparent;
  color: var(--fg);
  border:none;
  font-family: inherit;
  font-size: .95em;
  line-height: 1.35;
  padding: 10px;
}
.editor-textarea:focus{ outline:none; }
.editor-footer{
  background: var(--panel-bg2);
  border-top: 1px solid var(--border);
  padding: 6px 10px;
  font-size: .9em;
  display:flex;
  justify-content:space-between;
  align-items:center;
}
.editor-keys span{
  display:inline-block;
  padding: 0 4px;
  margin: 0 2px;
  background: var(--key-bg);
  color: var(--key-fg);
  border-radius: 0;
}
.editor-status{ color: var(--fg); opacity: .95; }

/* Inputs */
input[type="text"]{
  background: #191A22;
  color: var(--fg);
  border: 1px solid var(--border);
  padding: 8px;
  font-family: inherit;
  width: 100%;
}
input[type="text"]:focus{
  outline:none;
  border-color: var(--dr-cyan);
  box-shadow: 0 0 0 2px rgba(139,233,253,.18);
}

/* Responsive: keep the same logic, collapse like before */
@media (max-width: 600px){
  .main{ grid-template-rows: 1fr 1fr; }
  .panes{ flex-direction:column; margin: var(--frame-gap); }
  .pane + .pane{ border-left:none; border-top: 1px solid var(--border); }
  .ops{ grid-template-columns: 1fr; }
  .ops-panel + .ops-panel{ border-left:none; border-top: 1px solid var(--border); }
  .file-size{ display:none; }
  .queue-head.wpa, .queue-row.wpa{
    grid-template-columns: minmax(18ch, 1.7fr) minmax(9ch, 1fr) minmax(9ch, 1fr) 9ch;
  }
  .queue-head.wigle, .queue-row.wigle{
    grid-template-columns: minmax(18ch, 1fr) 6ch 9ch;
  }
}
)rawliteral";

static const char HTML_SCRIPT[] PROGMEM = R"rawliteral(

// Pane state
const DEFAULT_LEFT = '/m5porkchop/handshakes';
const DEFAULT_RIGHT = '/m5porkchop/wardriving';
const HANDSHAKES_DIR = '/m5porkchop/handshakes';
const WIGLE_DIR = '/m5porkchop/wardriving';
const panes = {
    L: { path: '/', items: [], selected: new Set(), focusIdx: 0, loading: false },
    R: { path: '/', items: [], selected: new Set(), focusIdx: 0, loading: false }
};
let activePane = 'L';
let sdInfoLoading = false;
let refreshInProgress = false;
let refreshPending = false;
let lastRefreshAt = 0;
let fetchQueue = Promise.resolve();
const LIST_LIMIT = 200;
let opsBusy = false;
let queueLoading = false;
const creds = {
    wpaKey: '',
    wigleUser: '',
    wigleToken: ''
};
let wpaQueue = [];
let wigleQueue = [];
let wpaResultsHandle = null;
let swineTimer = null;
let wpaAuthGateShown = false;
let wpaAuthState = 'REQUIRED';
let wpaAuthModalPromise = null;
let wpaAuthModalResolve = null;
const EDIT_MAX_BYTES = 2048;
let editPath = '';
let editDirty = false;
let editLoading = false;
const LOG_MAX = 5;
let logBuffer = [];

function queuedFetch(url, options) {
    const run = () => fetch(url, options);
    const p = fetchQueue.then(run, run);
    fetchQueue = p.catch(() => {});
    return p;
}

// Initialize
document.addEventListener('DOMContentLoaded', () => {
    document.addEventListener('keydown', handleKeydown);
    bootstrap();
});

async function bootstrap() {
    await loadConfigFromDevice();
    await loadPane('L', DEFAULT_LEFT);
    await loadPane('R', DEFAULT_RIGHT);
    await loadSDInfo();
    await loadSwine();
    if (!swineTimer) {
        swineTimer = setInterval(loadSwine, 15000);
    }
    initWpaPicker();
    await loadQueues();
    addSysLog('COMMANDER ONLINE');
}

function setActivePane(id) {
    activePane = id;
    document.getElementById('paneL').classList.toggle('active', id === 'L');
    document.getElementById('paneR').classList.toggle('active', id === 'R');
}

async function loadConfigFromDevice() {
    creds.wpaKey = '';
    creds.wigleUser = '';
    creds.wigleToken = '';
    try {
        const text = await fetchDeviceText('/m5porkchop/config/porkchop.conf');
        if (text) {
            const cfg = JSON.parse(text);
            if (cfg && cfg.wifi) {
                creds.wpaKey = (cfg.wifi.wpaSecKey || '').trim();
                creds.wigleUser = (cfg.wifi.wigleApiName || '').trim();
                creds.wigleToken = (cfg.wifi.wigleApiToken || '').trim();
            }
        }
    } catch(e) {
        // keep defaults
    }
    updateCredsStatus();
}

function updateCredsStatus() {
    const wpa = creds.wpaKey ? 'LOADED' : 'MISSING';
    const wigle = (creds.wigleUser && creds.wigleToken) ? 'LOADED' : 'MISSING';
    const wpaEl = document.getElementById('wpaMeta');
    const wigleEl = document.getElementById('wigleMeta');
    if (wpaEl) {
        wpaEl.textContent = 'KEY: ' + wpa;
    }
    if (wigleEl) wigleEl.textContent = 'CREDS: ' + wigle;
}

function setWpaAuthState(state) {
    wpaAuthState = state;
    updateCredsStatus();
}

function setOpsBusy(busy) {
    opsBusy = busy;
    const ids = ['btnWpaSync', 'btnWpaOpen', 'btnWigleSync'];
    ids.forEach(id => {
        const el = document.getElementById(id);
        if (el) el.disabled = busy;
    });
    const wpaPick = document.getElementById('wpaPick');
    if (wpaPick) wpaPick.disabled = busy;
}

function formatLogLine(source, msg) {
    const ts = new Date().toLocaleTimeString();
    return ts + ' [' + source + '] ' + msg;
}

function renderLogConsole() {
    const log = document.getElementById('logConsole');
    if (!log) return;
    log.textContent = logBuffer.length ? logBuffer.join('\n') : 'NO LOGS';
}

function pushLog(source, msg) {
    const line = formatLogLine(source, msg);
    logBuffer.push(line);
    while (logBuffer.length > LOG_MAX) {
        logBuffer.shift();
    }
    setStatus('[' + source + '] ' + msg);
    renderLogConsole();
}

function showLogConsole() {
    renderLogConsole();
    const modal = document.getElementById('logModal');
    if (modal) modal.style.display = 'flex';
}

function hideLogConsole() {
    const modal = document.getElementById('logModal');
    if (modal) modal.style.display = 'none';
}

function addWpaLog(msg) {
    pushLog('WPA', msg);
}

function addWigleLog(msg) {
    pushLog('WIGLE', msg);
}

function addSysLog(msg) {
    pushLog('SYS', msg);
}

async function loadSDInfo() {
    if (sdInfoLoading) return;
    sdInfoLoading = true;
    try {
        const r = await queuedFetch('/api/sdinfo');
        const d = await r.json();
        const pct = ((d.used / d.total) * 100).toFixed(0);
        const usedStr = formatSize(d.used * 1024);
        const totalStr = formatSize(d.total * 1024);
        document.getElementById('sdInfo').textContent = usedStr + ' / ' + totalStr + ' (' + pct + '%)';
        updateAllFooterDisk(usedStr, totalStr, pct);
    } catch(e) {
        document.getElementById('sdInfo').textContent = 'NO SD. NO LOOT.';
        updateAllFooterDisk('--', '--', '?');
    } finally {
        sdInfoLoading = false;
    }
}

function formatNumber(value) {
    try {
        return Number(value || 0).toLocaleString('en-US');
    } catch (e) {
        return String(value || 0);
    }
}

function renderSwineHeader(data) {
    if (!data) return;
    const line1 = document.getElementById('swineLine1');
    const line2 = document.getElementById('swineLine2');
    if (!line1 || !line2) return;

    const level = data.level || 0;
    const title = data.title || 'N0 D4TA';
    const titleMark = data.titleOverride ? '*' : '';
    const className = data.className || 'N0NE';
    const xpTotal = data.xpTotal || 0;
    const xpProgress = data.xpProgress || 0;
    const achUnlocked = (data.achUnlocked === undefined) ? 0 : data.achUnlocked;
    const achTotal = (data.achTotal === undefined) ? 0 : data.achTotal;

    line1.textContent = 
        'LV' + level + ' ' + title + titleMark +
        ' | T13R ' + className +
        ' | XP ' + formatNumber(xpTotal) + ' (' + xpProgress + '%)' +
        ' | B4DG3S ' + achUnlocked + '/' + achTotal;

    const wigleValid = !!data.wigleValid;
    const rank = data.wigleRank || 0;
    const wifi = data.wigleWifi || 0;
    const cell = data.wigleCell || 0;
    const bt = data.wigleBt || 0;
    const moodType = (data.moodType || 'BUFF').toUpperCase();
    const mood = data.mood || 'N0N3';
    const moodLabel = (moodType === 'NONE') ? 'BUFF: N0N3' : (moodType + ': ' + mood);

    if (wigleValid) {
        line2.textContent = 'W1GL3 #' + rank + ' | W:' + wifi + ' C:' + cell + ' B:' + bt + ' | ' + moodLabel;
    } else {
        line2.textContent = 'W1GL3 N/A | ' + moodLabel;
    }
}

async function loadSwine() {
    try {
        const r = await queuedFetch('/api/swine');
        if (!r.ok) return;
        const data = await r.json();
        renderSwineHeader(data);
    } catch (e) {
        // ignore
    }
}

async function loadPane(id, path) {
    const pane = panes[id];
    if (pane.loading) return;
    pane.loading = true;
    pane.path = path;
    pane.selected.clear();
    pane.focusIdx = 0;
    
    document.getElementById('path' + id).textContent = path || '/';
    const list = document.getElementById('list' + id);
    list.innerHTML = '<div style="padding:20px;opacity:0.5">jacking in...</div>';
    
    try {
        const r = await queuedFetch('/api/ls?dir=' + encodeURIComponent(path) + '&full=1&limit=' + LIST_LIMIT);
        const items = await r.json();
        
        // Sort: directories first, then alphabetically
        pane.items = [];
        
        // Parent directory entry
        if (path !== '/') {
            pane.items.push({ name: '..', isDir: true, isParent: true, size: 0 });
        }
        
        // Directories
        items.filter(i => i.isDir).sort((a,b) => a.name.localeCompare(b.name))
            .forEach(i => pane.items.push(i));
        
        // Files
        items.filter(i => !i.isDir).sort((a,b) => a.name.localeCompare(b.name))
            .forEach(i => pane.items.push(i));
        
        renderPane(id);
    } catch(e) {
        list.innerHTML = '<div style="padding:20px;opacity:0.5">load failed</div>';
    } finally {
        pane.loading = false;
        updateSelectionInfo(id);
    }
}

function renderPane(id) {
    const pane = panes[id];
    const list = document.getElementById('list' + id);
    
    if (pane.items.length === 0) {
        list.innerHTML = '<div style="padding:20px;opacity:0.4;text-align:center">void</div>';
        updatePaneFooter(id);
        return;
    }
    
    let html = '';
    pane.items.forEach((item, idx) => {
        const isSel = pane.selected.has(idx);
        const isFocus = (idx === pane.focusIdx && activePane === id);
        const cls = 'file-item' + (isSel ? ' selected' : '') + (isFocus ? ' focused' : '');
        
        // MC-style prefix: / for dirs, * for executables, space for regular files
        let prefix = ' ';
        let prefixCls = 'file-prefix file';
        let nameCls = 'file-name';
        if (item.isDir) {
            prefix = '/';
            prefixCls = 'file-prefix dir';
            nameCls = 'file-name dir';
        } else if (isExecutable(item.name)) {
            prefix = '*';
            prefixCls = 'file-prefix exec';
            nameCls = 'file-name exec';
        }
        
        // Size column: UP--DIR for parent, empty for dirs, size for files
        let size = '';
        if (item.isParent) {
            size = 'UP--DIR';
        } else if (!item.isDir) {
            size = formatSize(item.size);
        }
        
        // Time column: use mtime if available
        const time = item.isParent ? '' : (item.mtime ? formatMtime(item.mtime) : '');
        
        html += '<div class="' + cls + '" data-idx="' + idx + '" data-pane="' + id + '"';
        html += ' onclick="onItemClick(event,' + idx + ',\'' + id + '\')"';
        html += ' ondblclick="onItemDblClick(' + idx + ',\'' + id + '\')">';
        html += '<div class="' + prefixCls + '">' + prefix + '</div>';
        html += '<div class="' + nameCls + '">' + escapeHtml(item.name) + '</div>';
        html += '<div class="file-size">' + size + '</div>';
        html += '<div class="file-time">' + time + '</div>';
        html += '</div>';
    });
    list.innerHTML = html;
    
    // Scroll focused item into view
    const focused = list.querySelector('.focused');
    if (focused) focused.scrollIntoView({ block: 'nearest' });
    
    updatePaneFooter(id);
}

function isExecutable(name) {
    if (!name) return false;
    const lower = name.toLowerCase();
    return lower.endsWith('.sh') || lower.endsWith('.exe') || lower.endsWith('.bat') || lower.endsWith('.py');
}

function formatMtime(ts) {
    if (!ts) return '';
    const d = new Date(ts * 1000);
    const mon = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'][d.getMonth()];
    const day = String(d.getDate()).padStart(2, ' ');
    const hh = String(d.getHours()).padStart(2, '0');
    const mm = String(d.getMinutes()).padStart(2, '0');
    return mon + ' ' + day + ' ' + hh + ':' + mm;
}

function updatePaneFooter(id) {
    const pane = panes[id];
    const footerPath = document.getElementById('footerPath' + id);
    const footerDisk = document.getElementById('footerDisk' + id);
    if (footerPath) {
        footerPath.textContent = pane.path || '/';
    }
    // Disk info is updated separately via loadSDInfo
}

function updateAllFooterDisk(usedStr, totalStr, pct) {
    const diskText = usedStr + '/' + totalStr + ' (' + pct + '%)';
    const footerDiskL = document.getElementById('footerDiskL');
    const footerDiskR = document.getElementById('footerDiskR');
    if (footerDiskL) footerDiskL.textContent = diskText;
    if (footerDiskR) footerDiskR.textContent = diskText;
}

function onItemClick(event, idx, paneId) {
    setActivePane(paneId);
    panes[paneId].focusIdx = idx;
    
    if (event.ctrlKey || event.metaKey) {
        toggleSelect(paneId, idx);
    } else if (event.shiftKey) {
        // Range select not implemented for simplicity
        toggleSelect(paneId, idx);
    } else {
        renderPane(paneId);
    }
}

function onItemDblClick(idx, paneId) {
    const pane = panes[paneId];
    const item = pane.items[idx];
    
    if (item.isParent) {
        const parent = pane.path.substring(0, pane.path.lastIndexOf('/')) || '/';
        loadPane(paneId, parent);
    } else if (item.isDir) {
        const newPath = (pane.path === '/' ? '' : pane.path) + '/' + item.name;
        loadPane(paneId, newPath);
    } else {
        downloadFile(paneId, idx);
    }
}

function toggleSelect(paneId, idx) {
    const pane = panes[paneId];
    const item = pane.items[idx];
    if (item.isParent) return; // Can't select parent dir
    
    if (pane.selected.has(idx)) {
        pane.selected.delete(idx);
    } else {
        pane.selected.add(idx);
    }
    renderPane(paneId);
    updateSelectionInfo(paneId);
}

function updateSelectionInfo(id) {
    // Selection info now shown in status bar instead of removed pane elements
    const pane = panes[id];
    const count = pane.selected.size;
    if (count > 0) {
        setStatus('[' + count + ' SELECTED] | ↑↓ NAV | SPACE SEL | ENTER EXEC | TAB FLIP');
    }
}

function initWpaPicker() {
    const input = document.getElementById('wpaPick');
    if (!input) return;
    input.addEventListener('change', async () => {
        const file = input.files && input.files[0] ? input.files[0] : null;
        if (file) {
            setOpsBusy(true);
            await applyWpasecResultsFile(file);
            setOpsBusy(false);
        }
        input.value = '';
    });
}

function showWpaAuthModal() {
    if (wpaAuthModalPromise) return wpaAuthModalPromise;
    wpaAuthModalPromise = new Promise(resolve => {
        wpaAuthModalResolve = resolve;
        const modal = document.getElementById('wpaAuthModal');
        if (modal) modal.style.display = 'flex';
    });
    return wpaAuthModalPromise;
}

function closeWpaAuthModal(result) {
    const modal = document.getElementById('wpaAuthModal');
    if (modal) modal.style.display = 'none';
    if (wpaAuthModalResolve) {
        const resolve = wpaAuthModalResolve;
        wpaAuthModalResolve = null;
        resolve(result);
    }
    wpaAuthModalPromise = null;
}

function openWpaAuthTab() {
    if (!creds.wpaKey) {
        addWpaLog('AUTH BLOCKED: KEY MISSING IN /PORKCHOP.CONF');
        return;
    }
    const url = 'https://wpa-sec.stanev.org/?submit&key=' + encodeURIComponent(creds.wpaKey);
    window.open(url, '_blank');
    addWpaLog('AUTH TAB OPENED');
}

function handleWpaAuthAction(action) {
    if (action === 'auth') {
        openWpaAuthTab();
    }
    closeWpaAuthModal(action);
}

async function ensureWpaAuthGate(pendingCount) {
    if (pendingCount <= 0 || wpaAuthGateShown) return true;
    const result = await showWpaAuthModal();
    if (result === 'auth' || result === 'proceed') {
        wpaAuthGateShown = true;
        setWpaAuthState('ASSUMED');
        if (result === 'proceed') {
            addWpaLog('AUTH: PROCEED ANYWAY');
        }
        return true;
    }
    setWpaAuthState('REQUIRED');
    return false;
}

async function loadQueues() {
    if (queueLoading) return;
    queueLoading = true;
    try {
        wpaQueue = await buildWpaQueue();
        wigleQueue = await buildWigleQueue();
        renderWpaQueue();
        renderWigleQueue();
    } catch (e) {
        addSysLog('QUEUE LOAD FAILED: ' + describeError(e));
    } finally {
        queueLoading = false;
    }
}

async function listDir(path) {
    try {
        const r = await queuedFetch('/api/ls?dir=' + encodeURIComponent(path) + '&full=1&limit=' + LIST_LIMIT);
        if (!r.ok) return [];
        const items = await r.json();
        return Array.isArray(items) ? items : [];
    } catch (e) {
        return [];
    }
}

function stripExtension(name) {
    const dot = name.lastIndexOf('.');
    return dot > 0 ? name.substring(0, dot) : name;
}

function parseUploadedPaths(text) {
    const out = new Set();
    text.split(/\r?\n/).forEach(line => {
        const trimmed = line.trim();
        if (trimmed) out.add(trimmed);
    });
    return out;
}

function parseUploadedBssids(text) {
    const out = new Set();
    text.split(/\r?\n/).forEach(line => {
        const trimmed = line.trim();
        if (!trimmed) return;
        const bssid = normalizeBssid(trimmed);
        if (bssid.length >= 12) out.add(bssid);
    });
    return out;
}

function parseWpasecResultsMap(text) {
    const out = new Map();
    text.split(/\r?\n/).forEach(line => {
        const trimmed = line.trim();
        if (!trimmed || trimmed.startsWith('#')) return;
        if (trimmed.startsWith('WPA*')) {
            const lastColon = trimmed.lastIndexOf(':');
            if (lastColon <= 0) return;
            const hash = trimmed.substring(0, lastColon);
            const pass = trimmed.substring(lastColon + 1).trim();
            const parts = hash.split('*');
            if (parts.length >= 6) {
                const bssid = normalizeBssid(parts[3] || '');
                if (bssid.length < 12) return;
                const ssidHex = parts[5] || '';
                const ssid = decodeHexSSID(ssidHex);
                out.set(bssid, { ssid, pass });
            }
            return;
        }
        const first = trimmed.indexOf(':');
        const last = trimmed.lastIndexOf(':');
        if (first <= 0 || last <= first) return;
        const bssid = normalizeBssid(trimmed.substring(0, first));
        if (bssid.length < 12) return;
        const ssid = trimmed.substring(first + 1, last).trim();
        const pass = trimmed.substring(last + 1).trim();
        out.set(bssid, { ssid, pass });
    });
    return out;
}

function decodeHexSSID(hex) {
    if (!hex || hex.length < 2 || hex.length % 2 !== 0) return '';
    let out = '';
    for (let i = 0; i < hex.length; i += 2) {
        const byte = parseInt(hex.substr(i, 2), 16);
        if (isNaN(byte)) return '';
        if (byte === 0) continue;
        if (byte < 32 || byte > 126) {
            out += '.';
        } else {
            out += String.fromCharCode(byte);
        }
    }
    return out.trim();
}

async function buildWpaQueue() {
    const [items, uploadedText, resultsText] = await Promise.all([
        listDir(HANDSHAKES_DIR),
        fetchDeviceText('/m5porkchop/wpa-sec/wpasec_uploaded.txt'),
        fetchDeviceText('/m5porkchop/wpa-sec/wpasec_results.txt')
    ]);
    const uploadedSet = parseUploadedBssids(uploadedText);
    const resultsMap = parseWpasecResultsMap(resultsText);
    const queue = [];
    for (const item of items) {
        if (!item || item.isDir) continue;
        if (!item.name || !item.name.toLowerCase().endsWith('.pcap')) continue;
        let base = stripExtension(item.name);
        if (base.endsWith('_hs')) base = base.substring(0, base.length - 3);
        const bssidKey = normalizeBssid(base);
        const result = resultsMap.get(bssidKey);
        const ssid = (await readHandshakeSSID(base)) || (result ? result.ssid : '');
        const pass = result ? result.pass : '';
        let status = 'LOCAL';
        if (bssidKey && result) status = 'CRACKED';
        else if (bssidKey && uploadedSet.has(bssidKey)) status = 'UPLOADED';
        queue.push({
            path: HANDSHAKES_DIR + '/' + item.name,
            name: item.name,
            bssidKey,
            ssid: ssid || 'NONAME BRO',
            pass,
            status
        });
    }
    queue.sort((a, b) => a.name.localeCompare(b.name));
    return queue;
}

async function buildWigleQueue() {
    const [items, uploadedText] = await Promise.all([
        listDir(WIGLE_DIR),
        fetchDeviceText('/m5porkchop/wigle/wigle_uploaded.txt')
    ]);
    const uploadedSet = parseUploadedPaths(uploadedText);
    const queue = [];
    for (const item of items) {
        if (!item || item.isDir) continue;
        if (!item.name || !item.name.toLowerCase().endsWith('.wigle.csv')) continue;
        const path = WIGLE_DIR + '/' + item.name;
        const status = (uploadedSet.has(path) || uploadedSet.has(item.name)) ? 'UPLOADED' : 'LOCAL';
        const nets = await countWigleNetworks(path);
        queue.push({ path, name: item.name, nets, status });
    }
    queue.sort((a, b) => a.name.localeCompare(b.name));
    return queue;
}

async function readHandshakeSSID(baseName) {
    const path = HANDSHAKES_DIR + '/' + baseName + '.txt';
    const text = await fetchDeviceText(path);
    if (!text) return '';
    const line = text.split(/\r?\n/).find(l => l.trim());
    return line ? line.trim() : '';
}

function statusClassFor(status) {
    if (status === 'CRACKED') return 'status-cracked';
    if (status === 'UPLOADED') return 'status-ok';
    if (status === 'LOCAL') return 'status-local';
    return 'status-wait';
}

function renderWpaQueue() {
    const list = document.getElementById('wpaQueue');
    if (!list) return;
    if (!wpaQueue.length) {
        list.innerHTML = '<div class="queue-row wpa queue-dim"><div>--</div><div>--</div><div>--</div><div class="queue-status">EMPTY</div></div>';
        return;
    }
    let html = '';
    wpaQueue.forEach(item => {
        const dim = item.status === 'LOCAL' ? ' queue-dim' : '';
        const cls = statusClassFor(item.status);
        const pass = item.pass ? item.pass : '--';
        html += '<div class="queue-row wpa' + dim + '" title="' + escapeHtml(item.path) + '">';
        html += '<div>' + escapeHtml(item.name) + '</div>';
        html += '<div>' + escapeHtml(item.ssid) + '</div>';
        html += '<div>' + escapeHtml(pass) + '</div>';
        html += '<div class="queue-status ' + cls + '">' + item.status + '</div>';
        html += '</div>';
    });
    list.innerHTML = html;
}

function renderWigleQueue() {
    const list = document.getElementById('wigleQueue');
    if (!list) return;
    if (!wigleQueue.length) {
        list.innerHTML = '<div class="queue-row wigle queue-dim"><div>--</div><div>--</div><div class="queue-status">EMPTY</div></div>';
        return;
    }
    let html = '';
    wigleQueue.forEach(item => {
        const dim = item.status === 'LOCAL' ? ' queue-dim' : '';
        const cls = statusClassFor(item.status);
        const nets = (item.nets === undefined || item.nets === null) ? '?' : String(item.nets);
        html += '<div class="queue-row wigle' + dim + '" title="' + escapeHtml(item.path) + '">';
        html += '<div>' + escapeHtml(item.name) + '</div>';
        html += '<div>' + escapeHtml(nets) + '</div>';
        html += '<div class="queue-status ' + cls + '">' + item.status + '</div>';
        html += '</div>';
    });
    list.innerHTML = html;
}

function isWigleDataLine(line) {
    const trimmed = line.trim();
    if (!trimmed) return false;
    if (trimmed.startsWith('#')) return false;
    if (trimmed.startsWith('MAC,') || trimmed.startsWith('BSSID,')) return false;
    return true;
}

async function countWigleNetworks(path) {
    try {
        const resp = await queuedFetch('/download?f=' + encodeURIComponent(path));
        if (!resp.ok) return '?';
        if (!resp.body || !resp.body.getReader) {
            const text = await resp.text();
            let count = 0;
            text.split(/\r?\n/).forEach(line => { if (isWigleDataLine(line)) count++; });
            return count;
        }
        const reader = resp.body.getReader();
        const decoder = new TextDecoder();
        let carry = '';
        let count = 0;
        while (true) {
            const { value, done } = await reader.read();
            if (done) break;
            carry += decoder.decode(value, { stream: true });
            const lines = carry.split(/\r?\n/);
            carry = lines.pop() || '';
            lines.forEach(line => { if (isWigleDataLine(line)) count++; });
        }
        if (carry && isWigleDataLine(carry)) count++;
        return count;
    } catch (e) {
        return '?';
    }
}

function selectAll() {
    const pane = panes[activePane];
    pane.items.forEach((item, idx) => {
        if (!item.isParent) pane.selected.add(idx);
    });
    renderPane(activePane);
    updateSelectionInfo(activePane);
}

function handleKeydown(e) {
    // Don't handle if in modal input
    const activeTag = document.activeElement ? document.activeElement.tagName : '';
    if (activeTag === 'INPUT' || activeTag === 'TEXTAREA') return;
    const editModal = document.getElementById('editModal');
    if (editModal && editModal.style.display === 'flex') {
        if (e.key === 'Escape') {
            e.preventDefault();
            closeEditModal(false);
        }
        return;
    }
    const logModal = document.getElementById('logModal');
    if (logModal && logModal.style.display === 'flex') {
        if (e.key === 'Escape' || e.key === 'F10') {
            e.preventDefault();
            hideLogConsole();
        }
        return;
    }
    if (e.ctrlKey && (e.key === 'Enter' || e.key === 'NumpadEnter')) {
        e.preventDefault();
        downloadSelected();
        return;
    }
    
    const pane = panes[activePane];
    
    switch(e.key) {
        case 'ArrowUp':
            e.preventDefault();
            if (pane.focusIdx > 0) {
                pane.focusIdx--;
                renderPane(activePane);
            }
            break;
        case 'ArrowDown':
            e.preventDefault();
            if (pane.focusIdx < pane.items.length - 1) {
                pane.focusIdx++;
                renderPane(activePane);
            }
            break;
        case 'Enter':
            e.preventDefault();
            onItemDblClick(pane.focusIdx, activePane);
            break;
        case ' ':
            e.preventDefault();
            toggleSelect(activePane, pane.focusIdx);
            break;
        case 'Tab':
            e.preventDefault();
            setActivePane(activePane === 'L' ? 'R' : 'L');
            renderPane('L');
            renderPane('R');
            break;
        case 'Backspace':
            e.preventDefault();
            if (pane.path !== '/') {
                const parent = pane.path.substring(0, pane.path.lastIndexOf('/')) || '/';
                loadPane(activePane, parent);
            }
            break;
        case 'Delete':
            e.preventDefault();
            deleteSelected();
            break;
        case 'a':
            if (e.ctrlKey || e.metaKey) {
                e.preventDefault();
                selectAll();
            }
            break;
        case 'F1':
            e.preventDefault();
            showHelp();
            break;
        case 'F2':
            e.preventDefault();
            showRenameModal();
            break;
        case 'F3':
            e.preventDefault();
            refresh();
            break;
        case 'F4':
            e.preventDefault();
            showEditModal();
            break;
        case 'F5':
            e.preventDefault();
            copySelected();
            break;
        case 'F6':
            e.preventDefault();
            moveSelected();
            break;
        case 'F7':
            e.preventDefault();
            showNewFolderModal();
            break;
        case 'F8':
            e.preventDefault();
            deleteSelected();
            break;
        case 'F9':
            e.preventDefault();
            triggerUploadPicker();
            break;
        case 'F10':
            e.preventDefault();
            showLogConsole();
            break;
    }
}

function getSelectedPaths() {
    const paths = [];
    ['L', 'R'].forEach(id => {
        const pane = panes[id];
        pane.selected.forEach(idx => {
            const item = pane.items[idx];
            if (!item.isParent) {
                const path = (pane.path === '/' ? '' : pane.path) + '/' + item.name;
                paths.push({ path, isDir: item.isDir });
            }
        });
    });
    return paths;
}

async function deleteSelected() {
    const items = getSelectedPaths();
    if (items.length === 0) {
        addSysLog('SELECT TARGETS FIRST');
        return;
    }
    
    const msg = 'NUKE ' + items.length + ' ITEM(S)? NO UNDO. NO REGRETS.';
    if (!confirm(msg)) return;
    
    addSysLog('NUKING ' + items.length + ' TARGETS...');
    
    try {
        const resp = await queuedFetch('/api/bulkdelete', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ paths: items.map(i => i.path) })
        });
        const result = await resp.json();
        addSysLog('NUKED ' + result.deleted + '/' + items.length);
        refresh();
    } catch(e) {
        addSysLog('NUKE FAILED: ' + e.message);
    }
}

async function downloadSelected() {
    const items = getSelectedPaths().filter(i => !i.isDir);
    if (items.length === 0) {
        addSysLog('NO FILES MARKED. DIRS NEED ZIP. WE AINT GOT ZIP.');
        return;
    }
    
    addSysLog('EXFILTRATING ' + items.length + ' FILE(S)...');
    
    // Download files sequentially (browser limitation)
    for (let i = 0; i < items.length; i++) {
        await new Promise(resolve => {
            const a = document.createElement('a');
            a.href = '/download?f=' + encodeURIComponent(items[i].path);
            a.download = items[i].path.split('/').pop();
            a.click();
            setTimeout(resolve, 300); // Small delay between downloads
        });
    }
    
    addSysLog('EXFIL COMPLETE: ' + items.length);
}

function downloadFile(paneId, idx) {
    const pane = panes[paneId];
    const item = pane.items[idx];
    if (item.isDir) return;
    
    const path = (pane.path === '/' ? '' : pane.path) + '/' + item.name;
    window.location.href = '/download?f=' + encodeURIComponent(path);
}

async function refresh() {
    const now = Date.now();
    if (refreshInProgress) {
        refreshPending = true;
        return;
    }
    if (now - lastRefreshAt < 500) {
        return;
    }
    refreshInProgress = true;
    lastRefreshAt = now;
    await loadPane('L', panes.L.path);
    await loadPane('R', panes.R.path);
    await loadSDInfo();
    await loadSwine();
    await loadQueues();
    refreshInProgress = false;
    if (refreshPending) {
        refreshPending = false;
        refresh();
    }
}

function showNewFolderModal() {
    document.getElementById('newFolderModal').style.display = 'flex';
    document.getElementById('newFolderName').value = '';
    setTimeout(() => document.getElementById('newFolderName').focus(), 50);
}

function showHelp() {
    document.getElementById('helpModal').style.display = 'flex';
}

function hideModal() {
    document.getElementById('newFolderModal').style.display = 'none';
    document.getElementById('helpModal').style.display = 'none';
    document.getElementById('renameModal').style.display = 'none';
    const logModal = document.getElementById('logModal');
    if (logModal) logModal.style.display = 'none';
}

function getEditBytes(text) {
    return new TextEncoder().encode(text || '').length;
}

function limitEditBytes(text, maxBytes) {
    const enc = new TextEncoder();
    if (enc.encode(text).length <= maxBytes) return text;
    let end = text.length;
    while (end > 0 && enc.encode(text.slice(0, end)).length > maxBytes) {
        end--;
    }
    return text.slice(0, end);
}

function updateEditMeta() {
    const area = document.getElementById('editText');
    const meta = document.getElementById('editMeta');
    const status = document.getElementById('editStatus');
    if (!area || !meta || !status) return;
    const bytes = getEditBytes(area.value);
    meta.textContent = bytes + '/' + EDIT_MAX_BYTES + ' B';
    status.textContent = editDirty ? 'MODIFIED' : 'READY';
}

function closeEditModal(force) {
    if (!force && editDirty) {
        if (!confirm('DISCARD UNSAVED CHANGES?')) return;
    }
    const modal = document.getElementById('editModal');
    if (modal) modal.style.display = 'none';
    const area = document.getElementById('editText');
    if (area) {
        area.value = '';
        area.disabled = false;
    }
    editPath = '';
    editDirty = false;
    editLoading = false;
}

function handleEditKey(e) {
    if (e.ctrlKey && (e.key === 'o' || e.key === 'O' || e.key === 's' || e.key === 'S')) {
        e.preventDefault();
        saveEdit();
        return;
    }
    if (e.ctrlKey && (e.key === 'x' || e.key === 'X')) {
        e.preventDefault();
        closeEditModal(false);
        return;
    }
    if (e.key === 'Escape') {
        e.preventDefault();
        closeEditModal(false);
    }
}

function handleEditInput() {
    if (editLoading) return;
    const area = document.getElementById('editText');
    if (!area) return;
    const limited = limitEditBytes(area.value, EDIT_MAX_BYTES);
    if (limited !== area.value) {
        area.value = limited;
        addSysLog('EDIT: 2KB LIMIT');
    }
    editDirty = true;
    updateEditMeta();
}

async function showEditModal() {
    const pane = panes[activePane];
    const item = pane.items[pane.focusIdx];
    if (!item || item.isParent || item.isDir) { addSysLog('SELECT FILE TO EDIT'); return; }
    if (item.size > EDIT_MAX_BYTES) { addSysLog('EDIT: FILE TOO LARGE'); return; }

    const path = (pane.path === '/' ? '' : pane.path) + '/' + item.name;
    editPath = path;
    editDirty = false;
    editLoading = true;

    const modal = document.getElementById('editModal');
    const area = document.getElementById('editText');
    const title = document.getElementById('editTitle');
    const meta = document.getElementById('editMeta');
    const status = document.getElementById('editStatus');
    if (!modal || !area || !title || !meta || !status) return;

    title.textContent = 'EDIT - ' + item.name;
    meta.textContent = (item.size || 0) + '/' + EDIT_MAX_BYTES + ' B';
    status.textContent = 'LOADING...';
    area.value = '';
    area.disabled = true;
    modal.style.display = 'flex';

    try {
        const text = await fetchDeviceText(path);
        if (text.length > EDIT_MAX_BYTES) {
            addSysLog('EDIT: FILE TOO LARGE');
            closeEditModal(true);
            return;
        }
        area.value = text;
        area.disabled = false;
        area.focus();
        editLoading = false;
        updateEditMeta();
    } catch (e) {
        editLoading = false;
        addSysLog('EDIT LOAD FAILED: ' + describeError(e));
        closeEditModal(true);
    }
}

async function saveEdit() {
    if (editLoading) return;
    if (!editPath) { addSysLog('EDIT: NO FILE'); return; }
    const area = document.getElementById('editText');
    if (!area) return;
    let text = area.value || '';
    text = limitEditBytes(text, EDIT_MAX_BYTES);
    if (text !== area.value) area.value = text;
    const bytes = getEditBytes(text);
    if (bytes > EDIT_MAX_BYTES) {
        addSysLog('EDIT: 2KB LIMIT');
        return;
    }

    const tempPath = editPath + '.tmp';
    const backupPath = editPath + '.bak';
    const originalPath = editPath;
    let backupCreated = false;

    addSysLog('EDIT SAVING: ' + originalPath);

    try {
        await queuedFetch('/delete?f=' + encodeURIComponent(tempPath));
        await queuedFetch('/delete?f=' + encodeURIComponent(backupPath));
        await uploadTextToDevice(tempPath, text, 'text/plain');
        const renameOrig = await queuedFetch('/api/rename?old=' + encodeURIComponent(originalPath) + '&new=' + encodeURIComponent(backupPath));
        if (renameOrig.ok) backupCreated = true;
        const renameTemp = await queuedFetch('/api/rename?old=' + encodeURIComponent(tempPath) + '&new=' + encodeURIComponent(originalPath));
        if (!renameTemp.ok) throw new Error('RENAME FAILED');
        if (backupCreated) {
            await queuedFetch('/delete?f=' + encodeURIComponent(backupPath));
        }
        editDirty = false;
        updateEditMeta();
        closeEditModal(true);
        refresh();
        addSysLog('EDIT SAVED: ' + originalPath);
    } catch (e) {
        if (backupCreated) {
            await queuedFetch('/api/rename?old=' + encodeURIComponent(backupPath) + '&new=' + encodeURIComponent(originalPath));
        }
        await queuedFetch('/delete?f=' + encodeURIComponent(tempPath));
        addSysLog('EDIT SAVE FAILED: ' + describeError(e));
    }
}

function showRenameModal() {
    const pane = panes[activePane];
    const item = pane.items[pane.focusIdx];
    if (!item || item.isParent) { addSysLog('SELECT ITEM TO RENAME'); return; }
    const path = (pane.path === '/' ? '' : pane.path) + '/' + item.name;
    document.getElementById('renameOldPath').value = path;
    document.getElementById('renameNewName').value = item.name;
    document.getElementById('renameModal').style.display = 'flex';
    setTimeout(() => document.getElementById('renameNewName').select(), 50);
}

async function doRename() {
    const oldPath = document.getElementById('renameOldPath').value;
    const newName = document.getElementById('renameNewName').value.trim();
    if (!newName) { alert('PROVIDE NEW NAME'); return; }
    if (newName.includes('/') || newName.includes('..')) { alert('ILLEGAL CHARACTERS'); return; }
    
    const pane = panes[activePane];
    const newPath = (pane.path === '/' ? '' : pane.path) + '/' + newName;
    
    try {
        const resp = await queuedFetch('/api/rename?old=' + encodeURIComponent(oldPath) + '&new=' + encodeURIComponent(newPath));
        const result = await resp.json();
        if (result.success) {
            addSysLog('RENAMED: ' + newName);
            hideModal();
            loadPane(activePane, pane.path);
            loadQueues();
        } else {
            addSysLog('RENAME FAILED: ' + (result.error || 'UNKNOWN'));
        }
    } catch(e) {
        addSysLog('FAULT: ' + e.message);
    }
}

async function copySelected() {
    const src = panes[activePane];
    const dst = panes[activePane === 'L' ? 'R' : 'L'];
    
    // Prevent copying to same directory
    if (src.path === dst.path) {
        addSysLog('SOURCE AND DEST ARE SAME DIRECTORY');
        return;
    }
    
    // Get selected or focused items
    let items = [];
    src.selected.forEach(idx => {
        const item = src.items[idx];
        if (item && !item.isParent) items.push(item);
    });
    if (!items.length && src.focusIdx >= 0) {
        const item = src.items[src.focusIdx];
        if (item && !item.isParent) items = [item];
    }
    if (!items.length) { addSysLog('SELECT FILES TO COPY'); return; }
    
    const paths = items.map(i => (src.path === '/' ? '' : src.path) + '/' + i.name);
    addSysLog('COPYING ' + items.length + ' ITEM(S)...');
    
    try {
        const resp = await queuedFetch('/api/copy', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({files: paths, dest: dst.path})
        });
        const result = await resp.json();
        if (result.success) {
            addSysLog('COPIED: ' + result.copied + ' ITEM(S)');
            loadPane(activePane === 'L' ? 'R' : 'L', dst.path);
            loadQueues();
        } else {
            addSysLog('COPY FAILED: ' + (result.error || 'UNKNOWN'));
        }
    } catch(e) {
        addSysLog('FAULT: ' + e.message);
    }
}

async function moveSelected() {
    const src = panes[activePane];
    const dst = panes[activePane === 'L' ? 'R' : 'L'];
    
    // Prevent moving to same directory
    if (src.path === dst.path) {
        addSysLog('SOURCE AND DEST ARE SAME DIRECTORY');
        return;
    }
    
    // Get selected or focused items
    let items = [];
    src.selected.forEach(idx => {
        const item = src.items[idx];
        if (item && !item.isParent) items.push(item);
    });
    if (!items.length && src.focusIdx >= 0) {
        const item = src.items[src.focusIdx];
        if (item && !item.isParent) items = [item];
    }
    if (!items.length) { addSysLog('SELECT FILES TO MOVE'); return; }
    
    const paths = items.map(i => (src.path === '/' ? '' : src.path) + '/' + i.name);
    addSysLog('MOVING ' + items.length + ' ITEM(S)...');
    
    try {
        const resp = await queuedFetch('/api/move', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({files: paths, dest: dst.path})
        });
        const result = await resp.json();
        if (result.success) {
            addSysLog('MOVED: ' + result.moved + ' ITEM(S)');
            loadPane('L', panes.L.path);
            loadPane('R', panes.R.path);
            loadQueues();
        } else {
            addSysLog('MOVE FAILED: ' + (result.error || 'UNKNOWN'));
        }
    } catch(e) {
        addSysLog('FAULT: ' + e.message);
    }
}

async function createFolder() {
    const name = document.getElementById('newFolderName').value.trim();
    if (!name) { alert('NAME THE DIRECTORY'); return; }
    if (name.includes('/') || name.includes('..')) { alert('ILLEGAL CHARACTERS'); return; }
    
    const pane = panes[activePane];
    const path = (pane.path === '/' ? '' : pane.path) + '/' + name;
    
    try {
        const resp = await queuedFetch('/mkdir?f=' + encodeURIComponent(path));
        if (resp.ok) {
            addSysLog('SPAWNED: ' + name);
            hideModal();
            loadPane(activePane, pane.path);
        } else {
            addSysLog('SPAWN FAILED');
        }
    } catch(e) {
        addSysLog('FAULT: ' + e.message);
    }
}

async function uploadFiles(files) {
    if (!files || !files.length) return;
    
    const pane = panes[activePane];
    const bar = document.getElementById('progressBar');
    const fill = document.getElementById('progressFill');
    bar.classList.add('active');
    
    let uploaded = 0;
    for (let i = 0; i < files.length; i++) {
        addSysLog('INJECTING ' + (i+1) + '/' + files.length + ': ' + files[i].name);
        fill.style.width = '0%';
        
        const formData = new FormData();
        formData.append('file', files[i]);
        
        try {
            await new Promise((resolve, reject) => {
                const xhr = new XMLHttpRequest();
                xhr.upload.onprogress = (e) => {
                    if (e.lengthComputable) fill.style.width = (e.loaded/e.total*100) + '%';
                };
                xhr.onload = () => xhr.status === 200 ? resolve() : reject();
                xhr.onerror = () => reject();
                xhr.open('POST', '/upload?dir=' + encodeURIComponent(pane.path));
                xhr.send(formData);
            });
            uploaded++;
        } catch(e) {
            addSysLog('INJECT FAILED: ' + files[i].name);
        }
    }
    
    bar.classList.remove('active');
    addSysLog('INJECTED ' + uploaded + '/' + files.length + ' PAYLOADS');
    loadPane(activePane, pane.path);
    loadQueues();
    const input = document.getElementById('uploadPick');
    if (input) input.value = '';
}

function triggerUploadPicker() {
    const input = document.getElementById('uploadPick');
    if (input) {
        input.value = '';
        input.click();
    }
}

function normalizeBssid(raw) {
    return raw.replace(/[^a-fA-F0-9]/g, '').toUpperCase();
}

async function fetchDeviceBlob(path) {
    const resp = await queuedFetch('/download?f=' + encodeURIComponent(path));
    if (!resp.ok) throw new Error('device read failed (' + resp.status + ')');
    return await resp.blob();
}

async function fetchDeviceText(path) {
    const resp = await queuedFetch('/download?f=' + encodeURIComponent(path));
    if (!resp.ok) return '';
    return await resp.text();
}

async function uploadFileToDevice(dir, file) {
    const formData = new FormData();
    formData.append('file', file);
    const resp = await fetch('/upload?dir=' + encodeURIComponent(dir || '/'), {
        method: 'POST',
        body: formData
    });
    if (!resp.ok) throw new Error('device write failed (' + resp.status + ')');
}

async function uploadTextToDevice(path, text, mime) {
    const slash = path.lastIndexOf('/');
    const dir = slash <= 0 ? '/' : path.substring(0, slash);
    const name = slash < 0 ? path : path.substring(slash + 1);
    const file = new File([text], name, { type: mime || 'text/plain' });
    await uploadFileToDevice(dir, file);
}

function mergeLines(existingText, additions) {
    const set = new Set();
    existingText.split(/\r?\n/).forEach(line => {
        const trimmed = line.trim();
        if (trimmed) set.add(trimmed);
    });
    additions.forEach(line => {
        if (line) set.add(line);
    });
    return Array.from(set).join('\n') + '\n';
}

function describeError(e) {
    if (!e) return 'REQUEST FAILED';
    const msg = e.message || String(e);
    if (msg === 'Failed to fetch') return 'CORS OR NETWORK BLOCKED';
    return msg;
}

async function updateWpasecUploadedList(newBssid) {
    const path = '/m5porkchop/wpa-sec/wpasec_uploaded.txt';
    const bssid = normalizeBssid(newBssid || '');
    if (!bssid || bssid.length < 12) return;
    const existing = await fetchDeviceText(path);
    const existingSet = parseUploadedBssids(existing);
    const already = existingSet.has(bssid);
    if (already) return false;
    const merged = mergeLines(existing, [bssid]);
    await uploadTextToDevice(path, merged, 'text/plain');
    return true;
}

async function updateWpasecSentList(newBssid) {
    const path = '/m5porkchop/wpa-sec/wpasec_sent.txt';
    const bssid = normalizeBssid(newBssid || '');
    if (!bssid || bssid.length < 12) return;
    const existing = await fetchDeviceText(path);
    const existingSet = parseUploadedBssids(existing);
    const already = existingSet.has(bssid);
    if (already) return false;
    const merged = mergeLines(existing, [bssid]);
    await uploadTextToDevice(path, merged, 'text/plain');
    return true;
}

async function updateWpasecFromResults(text) {
    const resultsPath = '/m5porkchop/wpa-sec/wpasec_results.txt';
    await uploadTextToDevice(resultsPath, text, 'text/plain');
    const resultsMap = parseWpasecResultsMap(text);
    return resultsMap.size;
}

async function updateWigleUploadedList(fullPath) {
    const path = '/m5porkchop/wigle/wigle_uploaded.txt';
    const trimmed = (fullPath || '').trim();
    if (!trimmed) return;
    const existing = await fetchDeviceText(path);
    const existingSet = parseUploadedPaths(existing);
    const base = trimmed.split('/').pop() || trimmed;
    const already = existingSet.has(trimmed) || existingSet.has(base);
    if (already) return false;
    const merged = mergeLines(existing, [trimmed]);
    await uploadTextToDevice(path, merged, 'text/plain');
    return true;
}

async function wpaSync() {
    if (opsBusy) return;
    if (!creds.wpaKey) {
        addWpaLog('KEY MISSING IN /PORKCHOP.CONF');
        return;
    }
    const pendingPre = wpaQueue.filter(item => item.status === 'LOCAL');
    if (pendingPre.length) {
        const ok = await ensureWpaAuthGate(pendingPre.length);
        if (!ok) {
            addWpaLog('SYNC CANCELLED');
            return;
        }
    }
    setOpsBusy(true);
    await applyWpasecResultsAuto(true);
    const pending = wpaQueue.filter(item => item.status === 'LOCAL');
    if (pending.length) {
        addWpaLog('SYNC START: ' + pending.length + ' FILE(S)');
        let sent = 0;
        for (const item of pending) {
            const ok = await wpaUploadItem(item);
            if (ok) sent++;
        }
        addWpaLog('SYNC SENT: ' + sent + '/' + pending.length);
    } else {
        addWpaLog('SYNC: NOTHING PENDING');
    }
    const applied = await applyWpasecResultsAuto(false);
    setOpsBusy(false);
    if (applied && applied.error) {
        addWpaLog('APPLY FAIL: ' + applied.error);
    } else if (applied && applied.count !== undefined) {
        addWpaLog('APPLIED: ' + applied.count);
    } else if (applied && applied.pending) {
        addWpaLog('SELECT POTFILE TO APPLY');
    } else {
        addWpaLog('SYNC DONE');
    }
    await loadQueues();
}

function wpaOpenResults() {
    if (!creds.wpaKey) {
        addWpaLog('KEY MISSING IN /PORKCHOP.CONF');
        return;
    }
    const url = 'https://wpa-sec.stanev.org/?api&dl=1&key=' + encodeURIComponent(creds.wpaKey);
    window.open(url, '_blank');
    addWpaLog('RESULTS TAB OPENED');
}

async function applyWpasecResultsFile(file) {
    if (!file) return { count: 0 };
    addWpaLog('APPLY: ' + file.name);
    try {
        const text = await file.text();
        const count = await updateWpasecFromResults(text);
        addWpaLog('APPLIED: ' + count + ' ENTRIES');
        await loadQueues();
        return { count };
    } catch (e) {
        const msg = describeError(e);
        addWpaLog('APPLY FAIL: ' + msg);
        return { count: 0, error: msg };
    }
}

async function applyWpasecResultsAuto(allowPrompt) {
    try {
        if (window.showOpenFilePicker) {
            if (!wpaResultsHandle) {
                if (!allowPrompt) return { pending: true };
                const picks = await window.showOpenFilePicker({
                    multiple: false,
                    types: [{ description: 'WPA-SEC results', accept: { 'text/plain': ['.txt', '.potfile'] } }]
                });
                wpaResultsHandle = picks && picks.length ? picks[0] : null;
            }
            if (!wpaResultsHandle) {
                return { pending: true };
            }
            const file = await wpaResultsHandle.getFile();
            return await applyWpasecResultsFile(file);
        }
        const picker = document.getElementById('wpaPick');
        if (picker && allowPrompt) {
            picker.click();
            return { pending: true };
        }
    } catch (e) {
        const msg = describeError(e);
        wpaResultsHandle = null;
        addWpaLog('AUTO APPLY FAIL: ' + msg);
        return { error: msg };
    }
    return { pending: true };
}

async function wpaUploadItem(item) {
    addWpaLog('SEND: ' + item.name);
    try {
        const blob = await fetchDeviceBlob(item.path);
        const file = new File([blob], item.name, { type: 'application/octet-stream' });
        const url = 'https://wpa-sec.stanev.org/?submit&key=' + encodeURIComponent(creds.wpaKey);
        const form = new FormData();
        form.append('webfile', file);
        await fetch(url, { method: 'POST', body: form, mode: 'no-cors', credentials: 'include' });
        if (item.bssidKey && item.bssidKey.length >= 12) {
            await updateWpasecUploadedList(item.bssidKey);
            await updateWpasecSentList(item.bssidKey);
        } else {
            addWpaLog('NOTE: NO BSSID KEY FOR ' + item.name);
        }
        addWpaLog('SENT (BLIND): ' + item.name);
        return true;
    } catch (e) {
        const msg = describeError(e);
        addWpaLog('SEND FAIL: ' + item.name + ' - ' + msg);
        return false;
    }
}

async function wigleSync() {
    if (opsBusy) return;
    if (!creds.wigleUser || !creds.wigleToken) {
        addWigleLog('CREDS MISSING IN /PORKCHOP.CONF');
        return;
    }
    const pending = wigleQueue.filter(item => item.status === 'LOCAL');
    if (!pending.length) {
        addWigleLog('SYNC: NOTHING PENDING');
        return;
    }
    setOpsBusy(true);
    addWigleLog('SYNC START: ' + pending.length + ' FILE(S)');
    let okCount = 0;
    for (const item of pending) {
        const ok = await wigleUploadItem(item);
        if (ok) okCount++;
    }
    if (okCount > 0) {
        await wigleFetchStats();
        await loadSwine();
    }
    setOpsBusy(false);
    addWigleLog('SYNC DONE: ' + okCount + '/' + pending.length);
    await loadQueues();
}

async function wigleUploadItem(item) {
    addWigleLog('UPLOAD: ' + item.name);
    try {
        const blob = await fetchDeviceBlob(item.path);
        const file = new File([blob], item.name, { type: 'text/csv' });
        const auth = btoa(creds.wigleUser + ':' + creds.wigleToken);
        const form = new FormData();
        form.append('file', file);
        const resp = await fetch('https://api.wigle.net/api/v2/file/upload', {
            method: 'POST',
            headers: { 'Authorization': 'Basic ' + auth },
            body: form,
            mode: 'cors'
        });
        let data = null;
        try { data = await resp.json(); } catch (e) {}
        if (!resp.ok || !data || data.success !== true) {
            throw new Error((data && data.message) ? data.message : ('HTTP ' + resp.status));
        }
        await updateWigleUploadedList(item.path);
        addWigleLog('UPLOAD OK: ' + item.name);
        return true;
    } catch (e) {
        const msg = describeError(e);
        addWigleLog('UPLOAD FAIL: ' + item.name + ' - ' + msg);
        return false;
    }
}

async function wigleFetchStats() {
    try {
        const auth = btoa(creds.wigleUser + ':' + creds.wigleToken);
        const statsResp = await fetch('https://api.wigle.net/api/v2/stats/user', {
            headers: { 'Authorization': 'Basic ' + auth },
            mode: 'cors'
        });
        if (!statsResp.ok) {
            addWigleLog('STATS FAIL: HTTP ' + statsResp.status);
            return false;
        }
        const statsJson = await statsResp.json();
        const stats = {};
        stats.rank = statsJson.rank || (statsJson.statistics ? statsJson.statistics.rank : 0) || 0;
        const s = statsJson.statistics || {};
        stats.wifi = s.discoveredWiFi || s.wifiCount || 0;
        stats.cell = s.discoveredCell || s.cellCount || 0;
        stats.bt = s.discoveredBt || s.btCount || 0;
        await uploadTextToDevice('/m5porkchop/wigle/wigle_stats.json', JSON.stringify(stats), 'application/json');
        addWigleLog('STATS SAVED');
        return true;
    } catch (e) {
        addWigleLog('STATS FAIL: ' + describeError(e));
        return false;
    }
}

function setStatus(msg) {
    document.getElementById('status').textContent = msg;
}

function formatSize(bytes) {
    if (bytes < 1024) return bytes + 'B';
    if (bytes < 1024*1024) return (bytes/1024).toFixed(1) + 'K';
    if (bytes < 1024*1024*1024) return (bytes/1024/1024).toFixed(1) + 'M';
    return (bytes/1024/1024/1024).toFixed(2) + 'G';
}

function escapeHtml(s) {
    return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}


)rawliteral";
static const char HTML_TEMPLATE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>PC64.EXE</title>
    <link rel="stylesheet" href="/ui.css">
</head>
<body class="mc">
    <div class="header">
        <h1>PC64.EXE</h1>
        <div class="sd-info" id="sdInfo">...</div>
    </div>
    <div class="swine-strip">
        <div class="swine-line" id="swineLine1">LV0 N0 D4TA | T13R N0NE | XP 0 (0%) | B4DG3S 0/0</div>
        <div class="swine-line" id="swineLine2">W1GL3 N/A | BUFF: N0N3</div>
    </div>
    
    <div class="main">
        <div class="panes">
            <div class="pane active" id="paneL" onclick="setActivePane('L')">
                <div class="pane-header">
                    <span class="pane-decor-left">────────────────────────────────────────────────────────────────────────────────┤</span>
                    <span class="pane-path" id="pathL">/</span>
                    <span class="pane-decor-right">├────────────────────────────────────────────────────────────────────────────────</span>
                </div>
                <div class="col-header">
                    <div class="col-sort"></div>
                    <div class="col-name">Name</div>
                    <div class="col-size">Size</div>
                    <div class="col-time">Modif</div>
                </div>
                <div class="file-list" id="listL"></div>
                <div class="pane-footer">
                    <span class="pane-footer-decor-left">────────────────────────────────────────────────────────────────────────────────┤</span>
                    <span class="pane-footer-disk" id="footerDiskL"></span>
                    <span class="pane-footer-decor-right">├────────────────────────────────────────────────────────────────────────────────</span>
                </div>
            </div>
            <div class="pane" id="paneR" onclick="setActivePane('R')">
                <div class="pane-header">
                    <span class="pane-decor-left">────────────────────────────────────────────────────────────────────────────────┤</span>
                    <span class="pane-path" id="pathR">/</span>
                    <span class="pane-decor-right">├────────────────────────────────────────────────────────────────────────────────</span>
                </div>
                <div class="col-header">
                    <div class="col-sort"></div>
                    <div class="col-name">Name</div>
                    <div class="col-size">Size</div>
                    <div class="col-time">Modif</div>
                </div>
                <div class="file-list" id="listR"></div>
                <div class="pane-footer">
                    <span class="pane-footer-decor-left">────────────────────────────────────────────────────────────────────────────────┤</span>
                    <span class="pane-footer-disk" id="footerDiskR"></span>
                    <span class="pane-footer-decor-right">├────────────────────────────────────────────────────────────────────────────────</span>
                </div>
            </div>
        </div>
    <div class="ops">
        <div class="ops-panel">
            <div class="ops-block">
                <div class="ops-header">
                    <span class="ops-decor-left">────────────────────────────────────────────────────────────────────────────────┤</span>
                    <span class="ops-title-wrap">
                        <span class="ops-title">WPA-SEC QUEUE</span>
                        <span class="ops-meta" id="wpaMeta">KEY: UNKNOWN</span>
                        <span class="ops-actions">
                            <button class="btn btn-outline" id="btnWpaOpen" onclick="wpaOpenResults()">POT FILE</button>
                        </span>
                    </span>
                    <span class="ops-decor-right">├────────────────────────────────────────────────────────────────────────────────</span>
                </div>
                <div class="queue-head wpa">
                    <div>FILE</div><div>SSID</div><div>PASS</div><div class="queue-status">ST</div>
                </div>
                <div class="queue-list" id="wpaQueue"></div>
                <input type="file" id="wpaPick" accept=".txt,.potfile" style="display:none">
            </div>
        </div>
        <div class="ops-panel">
            <div class="ops-block">
                <div class="ops-header">
                    <span class="ops-decor-left">────────────────────────────────────────────────────────────────────────────────┤</span>
                    <span class="ops-title-wrap">
                        <span class="ops-title">WIGLE QUEUE</span>
                        <span class="ops-meta" id="wigleMeta">CREDS: UNKNOWN</span>
                        <span class="ops-actions">
                            <button class="btn" id="btnWigleSync" onclick="wigleSync()">SYNC</button>
                        </span>
                    </span>
                    <span class="ops-decor-right">├────────────────────────────────────────────────────────────────────────────────</span>
                </div>
                <div class="queue-head wigle">
                    <div>FILE</div><div>NETS</div><div class="queue-status">ST</div>
                </div>
                <div class="queue-list" id="wigleQueue"></div>
            </div>
        </div>
    </div>
    </div>
    
    <div class="progress-bar" id="progressBar"><div class="progress-fill" id="progressFill"></div></div>
    
    <div class="status" id="status">AWAITING ORDERS | ↑↓ NAV | SPACE SEL | ENTER EXEC | TAB FLIP</div>

    
    <div class="fkey-bar">
        <div class="fkey" onclick="showHelp()"><span>F1</span>?</div>
        <div class="fkey" onclick="showRenameModal()"><span>F2</span>REN</div>
        <div class="fkey" onclick="refresh()"><span>F3</span>REF</div>
        <div class="fkey" onclick="showEditModal()"><span>F4</span>EDT</div>
        <div class="fkey" onclick="copySelected()"><span>F5</span>CPY</div>
        <div class="fkey" onclick="moveSelected()"><span>F6</span>MOV</div>
        <div class="fkey" onclick="showNewFolderModal()"><span>F7</span>MKD</div>
        <div class="fkey" onclick="deleteSelected()"><span>F8</span>DEL</div>
        <div class="fkey" onclick="triggerUploadPicker()"><span>F9</span>PUT</div>
        <div class="fkey" onclick="showLogConsole()"><span>10</span>LOG</div>
    </div>
<input type="file" id="uploadPick" multiple onchange="uploadFiles(this.files)" style="display:none">
    
    <!-- New Folder Modal -->
    <div class="modal" id="newFolderModal" onclick="if(event.target===this)hideModal()">
        <div class="modal-content">
            <h3>NEW FOLDER</h3>
            <input type="text" id="newFolderName" placeholder="FOLDER NAME" 
                   onkeydown="if(event.key==='Enter')createFolder();if(event.key==='Escape')hideModal()">
            <div class="modal-actions">
                <button class="btn" onclick="createFolder()">CREATE</button>
                <button class="btn btn-outline" onclick="hideModal()">CANCEL</button>
            </div>
        </div>
    </div>
    
    <!-- Help Modal -->
    <div class="modal" id="helpModal" onclick="if(event.target===this)hideModal()">
        <div class="modal-content">
            <h3>KEYBOARD SHORTCUTS</h3>
            <pre style="font-size:0.85em;line-height:1.6;opacity:0.8">
ARROW UP/DOWN  NAVIGATE FILES
ENTER          OPEN FOLDER / DOWNLOAD
SPACE          TOGGLE SELECTION
TAB            SWITCH PANE
CTRL+A         SELECT ALL
F2             RENAME FOCUSED ITEM
F4             EDIT FILE (<=2KB)
F3             REFRESH
F5             COPY SEL → OTHER PANE
F6             MOVE SEL → OTHER PANE
F7             NEW FOLDER
F8/DELETE      DELETE SELECTED
F9             UPLOAD
F10            LOG CONSOLE
CTRL+ENTER     MULTI DOWNLOAD
BACKSPACE      PARENT FOLDER
            </pre>
            <div class="modal-actions">
                <button class="btn" onclick="hideModal()">CLOSE</button>
            </div>
        </div>
    </div>

    <!-- Log Console Modal -->
    <div class="modal" id="logModal" onclick="if(event.target===this)hideLogConsole()">
        <div class="log-console">
            <h3>LOG CONSOLE</h3>
            <pre id="logConsole">NO LOGS</pre>
            <div class="modal-actions">
                <button class="btn" onclick="hideLogConsole()">CLOSE</button>
            </div>
        </div>
    </div>

    <!-- Edit Modal -->
    <div class="modal" id="editModal" onclick="if(event.target===this)closeEditModal(false)">
        <div class="modal-content editor-content">
            <div class="editor-header">
                <div class="editor-title" id="editTitle">EDIT</div>
                <div class="editor-meta" id="editMeta">0/2048 B</div>
            </div>
            <div class="editor-body">
                <textarea class="editor-textarea" id="editText" onkeydown="handleEditKey(event)" oninput="handleEditInput()"></textarea>
            </div>
            <div class="editor-footer">
                <div class="editor-keys"><span>^O</span> SAVE  <span>^X</span> CANCEL</div>
                <div class="editor-status" id="editStatus">READY</div>
            </div>
        </div>
    </div>
    
    <!-- Rename Modal -->
    <div class="modal" id="renameModal" onclick="if(event.target===this)hideModal()">
        <div class="modal-content">
            <h3>RENAME</h3>
            <input type="text" id="renameNewName" placeholder="NEW NAME"
                   onkeydown="if(event.key==='Enter')doRename();if(event.key==='Escape')hideModal()">
            <input type="hidden" id="renameOldPath">
            <div class="modal-actions">
                <button class="btn" onclick="doRename()">RENAME</button>
                <button class="btn btn-outline" onclick="hideModal()">CANCEL</button>
            </div>
        </div>
    </div>

    <!-- WPA-SEC Auth Modal -->
    <div class="modal" id="wpaAuthModal" onclick="if(event.target===this)handleWpaAuthAction('cancel')">
        <div class="modal-content">
            <h3>WPA-SEC AUTH</h3>
            <div class="modal-body">
                AUTH ONCE IN A SEPARATE TAB, THEN RETURN HERE.<br>
                UPLOADS WITHOUT AUTH WON'T BE CREDITED.
            </div>
            <div class="modal-actions">
                <button class="btn" onclick="handleWpaAuthAction('auth')">AUTH NOW</button>
                <button class="btn btn-outline" onclick="handleWpaAuthAction('proceed')">PROCEED</button>
                <button class="btn btn-outline" onclick="handleWpaAuthAction('cancel')">CANCEL</button>
            </div>
        </div>
    </div>

<script src="/ui.js"></script>
</body>
</html>
)rawliteral";

void FileServer::init() {
    refreshSdPaths();
    state = FileServerState::IDLE;
    strcpy(statusMessage, "Ready");
    targetSSID[0] = '\0';
    targetPassword[0] = '\0';
    sessionRxBytes = 0;
    sessionTxBytes = 0;
    sessionUploadCount = 0;
    sessionDownloadCount = 0;
}

bool FileServer::start(const char* ssid, const char* password) {
    if (state != FileServerState::IDLE) {
        return true;
    }

    refreshSdPaths();
    
    // Store credentials for reconnection
    strncpy(targetSSID, ssid ? ssid : "", sizeof(targetSSID) - 1);
    targetSSID[sizeof(targetSSID) - 1] = '\0';
    strncpy(targetPassword, password ? password : "", sizeof(targetPassword) - 1);
    targetPassword[sizeof(targetPassword) - 1] = '\0';
    
    // Check credentials
    if (strlen(targetSSID) == 0) {
        strcpy(statusMessage, "No WiFi SSID set");
        return false;
    }

    // Pre-start heap conditioning if contiguous block is below TLS threshold.
    size_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largestBefore < HeapPolicy::kMinContigForTls) {
        Serial.printf("[FILESERVER] Pre-start conditioning: largest=%u < %u\n",
                      (unsigned)largestBefore, (unsigned)HeapPolicy::kMinContigForTls);
        size_t largestAfter = WiFiUtils::conditionHeapForTLS();
        Serial.printf("[FILESERVER] Pre-start conditioning complete: %u -> %u (+%d)\n",
                      (unsigned)largestBefore, (unsigned)largestAfter,
                      (int)(largestAfter - largestBefore));
    }
    
    strcpy(statusMessage, "jacking in.");
    logWiFiStatus("before connect");

    sessionRxBytes = 0;
    sessionTxBytes = 0;
    sessionUploadCount = 0;
    sessionDownloadCount = 0;
    
    // Start non-blocking connection (force restart to recover from desync)
    WiFiUtils::hardReset();
    WiFi.begin(targetSSID, targetPassword);
    
    state = FileServerState::CONNECTING;
    connectStartTime = millis();
    
    return true;
}

void FileServer::startServer() {
    // #region agent log
    Serial.println("[DBG-H1] startServer() ENTRY");
    // #endregion
    
    snprintf(statusMessage, sizeof(statusMessage), "%s", WiFi.localIP().toString().c_str());
    logWiFiStatus("startServer");
    
    // #region agent log
    Serial.printf("[DBG-H1] IP stored: %s\n", statusMessage);
    // #endregion
    
    // Start mDNS
    // #region agent log
    Serial.printf("[DBG-H1] WiFi connected=%d, localIP=%s\n", 
                  WiFi.isConnected(), WiFi.localIP().toString().c_str());
    Serial.println("[DBG-H1] About to call MDNS.begin()");
    // #endregion
    
    bool mdnsOk = MDNS.begin("porkchop");
    
    // #region agent log
    Serial.printf("[DBG-H1] MDNS.begin() returned %d\n", mdnsOk);
    // #endregion
    
    // #region agent log
    Serial.println("[DBG-H2] MDNS.begin() completed, creating WebServer");
    // #endregion
    
    // FIX: Heap guard before WebServer allocation - prevent OOM on ADV/tight heap
    {
        size_t freeHeap = ESP.getFreeHeap();
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        if (freeHeap < HeapPolicy::kFileServerMinHeap || largest < HeapPolicy::kFileServerMinLargest) {
            Serial.printf("[FILESERVER] Low heap for WebServer: free=%u largest=%u\n",
                          (unsigned)freeHeap, (unsigned)largest);
            strcpy(statusMessage, "Low heap");
            MDNS.end();  // Clean up mDNS we just started
            WiFiUtils::shutdown();
            state = FileServerState::IDLE;
            return;
        }
    }
    
    // Create and configure web server
    server = new WebServer(80);
    
    // #region agent log
    Serial.printf("[DBG-H2] WebServer created, ptr=%p\n", (void*)server);
    // #endregion
    
    if (!server) {
        // #region agent log
        Serial.println("[DBG-H2] ERROR: WebServer allocation failed!");
        // #endregion
        strcpy(statusMessage, "Server alloc fail");
        state = FileServerState::IDLE;
        return;
    }
    
    // #region agent log
    Serial.println("[DBG-H3] Registering handlers...");
    // #endregion
    
    server->on("/", HTTP_GET, handleRoot);
    server->on("/ui.css", HTTP_GET, handleStyle);
    server->on("/ui.js", HTTP_GET, handleScript);
    server->on("/api/swine", HTTP_GET, handleSwine);
    server->on("/api/ls", HTTP_GET, handleFileList);
    server->on("/api/sdinfo", HTTP_GET, handleSDInfo);
    server->on("/api/bulkdelete", HTTP_POST, handleBulkDelete);
    server->on("/api/rename", HTTP_GET, handleRename);
    server->on("/api/copy", HTTP_POST, handleCopy);
    server->on("/api/move", HTTP_POST, handleMove);
    server->on("/download", HTTP_GET, handleDownload);
    server->on("/upload", HTTP_POST, handleUpload, handleUploadProcess);
    server->on("/delete", HTTP_GET, handleDelete);
    server->on("/rmdir", HTTP_GET, handleDelete);  // Same handler, will detect folder
    server->on("/mkdir", HTTP_GET, handleMkdir);
    server->onNotFound(handleNotFound);
    
    // #region agent log
    Serial.println("[DBG-H3] Handlers registered, calling server->begin()");
    // #endregion
    
    server->begin();
    
    // #region agent log
    Serial.println("[DBG-H4] server->begin() completed");
    // #endregion
    
    state = FileServerState::RUNNING;
    lastReconnectCheck = millis();

    xpLastScanMs = 0;
    xpLastUploadCount = sessionUploadCount;
    xpSessionAwarded = 0;
    xpScanPending = true;
    xpAwardedWpa.clear();
    xpAwardedWigle.clear();
    xpWpaLoaded = false;
    xpWigleLoaded = false;
    xpWpaCacheComplete = false;
    xpWigleCacheComplete = false;
}

void FileServer::stop() {
    if (state == FileServerState::IDLE) {
        return;
    }
    // Close any pending upload file
    resetUploadState(false);

    scanXpAwards();
    
    if (server) {
        server->stop();
        delete server;
        server = nullptr;
    }
    
    MDNS.end();
    WiFiUtils::shutdown();
    
    // Heap recovery: brief promiscuous cycle to coalesce fragments
    // WebServer + mDNS allocations cause fragmentation; this triggers WiFi driver's
    // internal buffer reorganization which helps defragment the heap
    size_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largestBefore < HeapPolicy::kFileServerRecoveryThreshold) {
        // Short callback-enabled brew for reliable coalescing
        Serial.printf("[FILESERVER] Heap recovery starting: largest=%u\n", (unsigned)largestBefore);
        size_t largestAfter = WiFiUtils::brewHeap(2000, false);
        if (largestAfter < HeapPolicy::kMinContigForTls) {
            Serial.printf("[FILESERVER] Brew insufficient (largest=%u), running full conditioning\n",
                          (unsigned)largestAfter);
            largestAfter = WiFiUtils::conditionHeapForTLS();
        }
        Serial.printf("[FILESERVER] Heap recovery complete: %u -> %u (+%d)\n",
                      (unsigned)largestBefore, (unsigned)largestAfter,
                      (int)(largestAfter - largestBefore));
    }
    
    state = FileServerState::IDLE;
    strcpy(statusMessage, "OFFLINE");
    sessionRxBytes = 0;
    sessionTxBytes = 0;
    sessionUploadCount = 0;
    sessionDownloadCount = 0;
    xpSessionAwarded = 0;
    xpScanPending = false;
    // FIX: Purge vectors to free heap, prevent unbounded growth
    xpAwardedWpa.clear();
    xpAwardedWpa.shrink_to_fit();
    xpAwardedWigle.clear();
    xpAwardedWigle.shrink_to_fit();
    xpWpaLoaded = false;
    xpWigleLoaded = false;
    xpWpaCacheComplete = false;
    xpWigleCacheComplete = false;
}

void FileServer::update() {
    switch (state) {
        case FileServerState::CONNECTING:
        case FileServerState::RECONNECTING:
            updateConnecting();
            break;
        case FileServerState::RUNNING:
            updateRunning();
            break;
        default:
            break;
    }
}

void FileServer::updateConnecting() {
    uint32_t elapsed = millis() - connectStartTime;
    
    if (WiFi.status() == WL_CONNECTED) {
        startServer();
        return;
    }
    
    // Update status with dots animation
    int dots = (elapsed / 500) % 4;
    snprintf(statusMessage, sizeof(statusMessage), "jacking in%.*s", dots, "...");
    
    // Timeout after 15 seconds
    if (elapsed > 15000) {
        strcpy(statusMessage, "LINK FAILED");
        logWiFiStatus("connect timeout");
        WiFiUtils::shutdown();
        state = FileServerState::IDLE;
    }
}

void FileServer::updateRunning() {
    if (!XP_WPA_AWARDED_FILE || !XP_WIGLE_AWARDED_FILE) {
        refreshSdPaths();
    }
    if (server) {
        server->handleClient();
    }

    if (uploadActive.load() && (millis() - uploadLastProgress.load() > 10000)) {
        resetUploadState(true);
    }
    
    // FIX: Timeout safety for listActive - prevent permanent lockout
    if (listActive.load() && (millis() - listStartTime.load() > 60000)) {
        Serial.println("[FILESERVER] List operation timeout, resetting listActive");
        listActive.store(false);
    }

    if (sessionUploadCount != xpLastUploadCount) {
        xpLastUploadCount = sessionUploadCount;
        xpScanPending = true;
    }

    uint32_t now = millis();
    if (xpScanPending && (xpLastScanMs == 0 || (now - xpLastScanMs) > 1000)) {
        xpLastScanMs = now;
        xpScanPending = false;
        scanXpAwards();
    }
    
    // Check WiFi connection every 5 seconds
    if (now - lastReconnectCheck > 5000) {
        lastReconnectCheck = now;
        
        if (WiFi.status() != WL_CONNECTED) {
            strcpy(statusMessage, "retry hack.");
            logWiFiStatus("wifi lost");

            if (uploadActive.load()) {
                resetUploadState(true);
            }
            
            // Stop server but keep credentials
            if (server) {
                server->stop();
                delete server;
                server = nullptr;
            }
            
            // Stop mDNS before reconnect
            MDNS.end();
            
            // Restart connection
            WiFiUtils::hardReset();
            WiFi.begin(targetSSID, targetPassword);
            state = FileServerState::RECONNECTING;
            connectStartTime = millis();
        }
    }
}

uint64_t FileServer::getSDFreeSpace() {
    return SD.totalBytes() - SD.usedBytes();
}

uint64_t FileServer::getSDTotalSpace() {
    return SD.totalBytes();
}

void FileServer::handleRoot() {
    logRequest(server, "REQ");
    if (isTransferBusy()) {
        sendBusyResponse(server);
        return;
    }
    logHeapStatus("before /");
    size_t freeHeap = 0;
    size_t largest = 0;
    if (isUiHeapLow(&freeHeap, &largest)) {
        Serial.printf("[FILESERVER] Low heap for UI: free=%u largest=%u\n",
                      (unsigned int)freeHeap, (unsigned int)largest);
        size_t sent = sendProgmemResponse(server, 503, "text/html; charset=utf-8", LOW_HEAP_PAGE);
        sessionTxBytes += sent;
        return;
    }
    size_t sent = sendProgmemResponse(server, "text/html; charset=utf-8", HTML_TEMPLATE);
    sessionTxBytes += sent;
    logHeapStatus("after /");
}

void FileServer::handleStyle() {
    logRequest(server, "REQ");
    if (isTransferBusy()) {
        sendBusyResponse(server);
        return;
    }
    size_t freeHeap = 0;
    size_t largest = 0;
    if (isUiHeapLow(&freeHeap, &largest)) {
        Serial.printf("[FILESERVER] Low heap for CSS: free=%u largest=%u\n",
                      (unsigned int)freeHeap, (unsigned int)largest);
        server->sendHeader("Connection", "close");
        server->send(503, "text/plain", "LOW HEAP");
        return;
    }
    size_t sent = sendProgmemResponse(server, "text/css", HTML_STYLE);
    sessionTxBytes += sent;
}

void FileServer::handleScript() {
    logRequest(server, "REQ");
    if (isTransferBusy()) {
        sendBusyResponse(server);
        return;
    }
    size_t freeHeap = 0;
    size_t largest = 0;
    if (isUiHeapLow(&freeHeap, &largest)) {
        Serial.printf("[FILESERVER] Low heap for JS: free=%u largest=%u\n",
                      (unsigned int)freeHeap, (unsigned int)largest);
        server->sendHeader("Connection", "close");
        server->send(503, "text/plain", "LOW HEAP");
        return;
    }
    size_t sent = sendProgmemResponse(server, "application/javascript", HTML_SCRIPT);
    sessionTxBytes += sent;
}

void FileServer::handleSwine() {
    logRequest(server, "REQ");
    if (isTransferBusy()) {
        sendBusyResponse(server);
        return;
    }
    const char* json = buildSwineSummaryJson();  // FIX: No heap allocation
    server->sendHeader("Connection", "close");
    server->sendHeader("Cache-Control", "no-store");
    server->send(200, "application/json", json);
    sessionTxBytes += strlen(json);
}

void FileServer::handleSDInfo() {
    logRequest(server, "REQ");
    if (isTransferBusy()) {
        sendBusyResponse(server);
        return;
    }
    logHeapStatusIfLow("before /api/sdinfo");
    
    // Calculate SD card info with error handling
    uint64_t totalBytes = 0;
    uint64_t usedBytes = 0;
    
    // Safely get SD card statistics
    if (SD.cardType() == CARD_NONE) {
        server->sendHeader("Connection", "close");
        server->send(500, "application/json", "{\"error\":\"No SD card\"}");
        return;
    }
    
    totalBytes = SD.totalBytes();
    usedBytes = SD.usedBytes();
    
    // FIX: Use snprintf to avoid String temp allocations
    char json[96];
    snprintf(json, sizeof(json), "{\"total\":%lu,\"used\":%lu,\"free\":%lu}",
             (unsigned long)(totalBytes / 1024),
             (unsigned long)(usedBytes / 1024),
             (unsigned long)((totalBytes - usedBytes) / 1024));
    
    server->sendHeader("Connection", "close");
    server->send(200, "application/json", json);
    sessionTxBytes += strlen(json);
    logHeapStatusIfLow("after /api/sdinfo");
}

void FileServer::handleFileList() {
    String dir = mapUiPathToFs(server->arg("dir"));
    bool full = server->arg("full") == "1";
    uint16_t limit = server->arg("limit").toInt();
    logRequest(server, "REQ");
    if (listActive.load() || isTransferBusy()) {
        sendBusyResponse(server);
        return;
    }
    listActive.store(true);
    listStartTime.store(millis());  // FIX: Atomic store for cross-context safety
    if (limit == 0 || limit > 1000) {
        limit = 200;
    }
    if (dir.isEmpty()) dir = "/";
    logHeapStatusIfLow("before /api/ls");
    
    // Security: prevent directory traversal
    if (dir.indexOf("..") >= 0) {
        server->sendHeader("Connection", "close");
        server->send(400, "application/json", "[]");
        listActive.store(false);
        return;
    }
    
    File root = SD.open(dir);
    if (!root || !root.isDirectory()) {
        server->sendHeader("Connection", "close");
        server->send(200, "application/json", "[]");
        listActive.store(false);
        return;
    }

    WiFiClient client = server->client();
    client.setNoDelay(true);

    server->sendHeader("Connection", "close");
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, "application/json", "[");

    bool first = true;
    uint16_t sentCount = 0;
    String buffer;
    buffer.reserve(1024);

    File file = root.openNextFile();
    while (file && sentCount < limit) {
        yield(); // Feed watchdog during file operations
        
        // Check if client is still connected before continuing
        if (!client.connected()) {
            file.close();
            root.close();
            listActive.store(false);
            return;
        }

        // FIX: Use stack buffer for number formatting to avoid heap allocs in loop
        char numBuf[16];
        
        if (!first) {
            buffer += ",";
        }
        buffer += "{\"name\":\"";
        const char* baseName = basenameFromPath(file.name());
        appendJsonEscaped(buffer, baseName);
        buffer += "\",\"size\":";
        snprintf(numBuf, sizeof(numBuf), "%u", (unsigned)file.size());
        buffer += numBuf;
        if (full) {
            buffer += ",\"isDir\":";
            buffer += file.isDirectory() ? "true" : "false";
            time_t t = file.getLastWrite();
            if (t > 0) {
                buffer += ",\"mtime\":";
                snprintf(numBuf, sizeof(numBuf), "%lu", (unsigned long)t);
                buffer += numBuf;
            }
        }
        buffer += "}";

        if (buffer.length() >= 1024) {
            server->sendContent(buffer);
            sessionTxBytes += buffer.length();
            buffer.remove(0);  // FIX: Clear in-place to keep capacity, avoid realloc
            if (!client.connected()) {
                file.close();
                root.close();
                listActive.store(false);
                return;
            }
        }

        first = false;
        sentCount++;
        file.close();
        file = root.openNextFile();
    }

    if (file) {
        file.close();
    }
    root.close();

    buffer += "]";
    server->sendContent(buffer);
    sessionTxBytes += buffer.length();
    server->sendContent("");  // Finalize chunked transfer
    client.flush();
    client.stop();
    logHeapStatusIfLow("after /api/ls");
    listActive.store(false);
}

void FileServer::handleDownload() {
    String path = mapUiPathToFs(server->arg("f"));
    String dir = mapUiPathToFs(server->arg("dir"));  // For ZIP download
    logRequest(server, "REQ");
    logHeapStatusIfLow("before /download");
    
    // ZIP download of folder
    if (!dir.isEmpty()) {
        // Simple implementation: send files one by one is not possible
        // Instead, we'll create a simple text manifest for now
        // Full ZIP requires external library
        server->send(501, "text/plain", "ZIP download not yet implemented - download files individually");
        return;
    }
    
    if (uploadActive.load()) {
        server->sendHeader("Connection", "close");
        server->send(409, "text/plain", "Upload in progress");
        return;
    }

    if (path.isEmpty()) {
        server->sendHeader("Connection", "close");
        server->send(400, "text/plain", "Missing file path");
        return;
    }
    
    // Security: prevent directory traversal
    if (path.indexOf("..") >= 0) {
        server->sendHeader("Connection", "close");
        server->send(400, "text/plain", "Invalid path");
        return;
    }
    
    File file = SD.open(path);
    if (!file || file.isDirectory()) {
        server->sendHeader("Connection", "close");
        server->send(404, "text/plain", "File not found");
        return;
    }
    
    // FIX: Use const char* / char[] instead of String to avoid heap allocs
    // Get filename for Content-Disposition
    const char* pathCStr = path.c_str();
    const char* filename = pathCStr;
    const char* lastSlash = strrchr(pathCStr, '/');
    if (lastSlash) {
        filename = lastSlash + 1;
    }
    
    // Determine content type (use const char* - no allocation)
    const char* contentType = "application/octet-stream";
    if (path.endsWith(".txt")) contentType = "text/plain";
    else if (path.endsWith(".csv")) contentType = "text/csv";
    else if (path.endsWith(".json")) contentType = "application/json";
    else if (path.endsWith(".pcap")) contentType = "application/vnd.tcpdump.pcap";
    
    const size_t totalSize = file.size();
    
    // FIX: Build Content-Disposition header in stack buffer to avoid String concat
    char dispositionBuf[160];
    snprintf(dispositionBuf, sizeof(dispositionBuf), "attachment; filename=\"%s\"", filename);
    
    server->sendHeader("Connection", "close");
    server->sendHeader("Content-Disposition", dispositionBuf);
    server->setContentLength(totalSize);
    server->send(200, contentType, "");

    WiFiClient client = server->client();
    client.setNoDelay(true);

    static uint8_t buffer[1024];
    size_t sentTotal = 0;
    uint32_t lastProgress = millis();
    bool stalled = false;

    while (sentTotal < totalSize && client.connected()) {
        yield(); // Feed watchdog during long operations

        // Check for timeout
        if (millis() - lastProgress > 30000) {  // 30 second timeout
            stalled = true;
            break;
        }

        size_t toRead = totalSize - sentTotal;
        if (toRead > sizeof(buffer)) {
            toRead = sizeof(buffer);
        }

        size_t readBytes = file.read(buffer, toRead);
        if (readBytes == 0) {
            break;
        }

        size_t offset = 0;
        while (offset < readBytes && client.connected()) {
            size_t chunk = readBytes - offset;
            int avail = client.availableForWrite();
            if (avail > 0 && chunk > static_cast<size_t>(avail)) {
                chunk = static_cast<size_t>(avail);
            }

            size_t written = client.write(buffer + offset, chunk);
            if (written == 0) {
                if (millis() - lastProgress > 8000) {
                    stalled = true;
                    break;
                }
                delay(1);
                yield();
                continue;
            }

            offset += written;
            sentTotal += written;
            lastProgress = millis();
        }

        if (stalled) {
            break;
        }
    }

    client.flush();
    client.stop();
    file.close();

    if (sentTotal > 0) {
        sessionTxBytes += sentTotal;
        sessionDownloadCount++;
    }

    logHeapStatusIfLow("after /download");
}

void FileServer::handleUpload() {
    logRequest(server, "REQ");
    if (uploadRejected.load()) {
        server->sendHeader("Connection", "close");
        server->send(409, "text/plain", "Transfer in progress");
        uploadRejected.store(false);
        return;
    }
    server->sendHeader("Connection", "close");
    server->send(200, "text/plain", "OK");
}

void FileServer::handleUploadProcess() {
    HTTPUpload& upload = server->upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        if (uploadActive.load()) {
            uploadRejected.store(true);
            return;
        }
        uploadRejected.store(false);
        uploadActive.store(true);
        uploadLastProgress.store(millis());
        
        // FIX: Parse dir arg ONLY on UPLOAD_FILE_START (was incorrectly accessible on every chunk)
        // Use char buffer instead of String to avoid heap fragmentation
        {
            String dirArg = mapUiPathToFs(server->arg("dir"));
            if (dirArg == "/" || dirArg.isEmpty()) {
                uploadDirBuf[0] = '\0';
            } else {
                size_t dirLen = dirArg.length();
                if (dirLen >= sizeof(uploadDirBuf) - 1) {
                    dirLen = sizeof(uploadDirBuf) - 2;  // Leave room for potential '/' and null
                }
                strncpy(uploadDirBuf, dirArg.c_str(), dirLen);
                uploadDirBuf[dirLen] = '\0';
                // Add trailing slash if needed
                if (dirLen > 0 && uploadDirBuf[dirLen - 1] != '/') {
                    uploadDirBuf[dirLen] = '/';
                    uploadDirBuf[dirLen + 1] = '\0';
                }
            }
        }  // dirArg String freed here before SD operations
        
        // Security: prevent directory traversal
        const char* filename = upload.filename.c_str();
        if (strstr(filename, "..") != nullptr || strstr(uploadDirBuf, "..") != nullptr) {
            uploadRejected.store(true);
            uploadActive.store(false);
            return;
        }
        
        // Build upload path in fixed buffer
        if (uploadDirBuf[0] == '\0') {
            snprintf(uploadPathBuf, sizeof(uploadPathBuf), "/%s", filename);
        } else {
            snprintf(uploadPathBuf, sizeof(uploadPathBuf), "%s%s", uploadDirBuf, filename);
        }
        logHeapStatusIfLow("before upload");
        
        uploadFile = SD.open(uploadPathBuf, FILE_WRITE);
        if (!uploadFile) {
            resetUploadState(false);
            uploadRejected.store(true);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) {
            size_t written = uploadFile.write(upload.buf, upload.currentSize);
            if (written != upload.currentSize) {
                Serial.printf("[FILESERVER] Upload write failed: wrote %u/%u\n",
                              (unsigned int)written, (unsigned int)upload.currentSize);
                resetUploadState(true);
                return;
            }
            sessionRxBytes += upload.currentSize;
            uploadLastProgress.store(millis());
            yield(); // Feed watchdog during upload
        } else {
            // Safety check: if uploadFile is not open, reject the upload
            Serial.println("[FILESERVER] Upload write attempted but no file open");
            resetUploadState(true);
            return;
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uploadFile.close();
            sessionUploadCount++;
        }
        resetUploadState(false);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        // Client disconnected or error - close file to prevent leak
        resetUploadState(true);
    }
    
    // Additional safety check: if upload takes too long, reset to prevent hanging
    if (uploadActive.load() && (millis() - uploadLastProgress.load() > 30000)) {  // 30 second timeout
        Serial.println("[FILESERVER] Upload timeout detected, resetting upload state");
        resetUploadState(true);
    }
}

// FIX: Max recursion depth to prevent stack overflow on ESP32 (~8KB stack)
static const uint8_t MAX_RECURSION_DEPTH = 12;

// FIX: Shared yield state for recursive operations - ensures regular yields during deep recursion
static uint32_t recursiveOpLastYield = 0;
static uint16_t recursiveOpCounter = 0;

static void recursiveYieldCheck() {
    // Yield every 4 operations OR every 50ms, whichever comes first
    recursiveOpCounter++;
    if (recursiveOpCounter >= 4 || (millis() - recursiveOpLastYield) > 50) {
        yield();
        recursiveOpLastYield = millis();
        recursiveOpCounter = 0;
    }
}

// Internal recursive delete with depth tracking
// FIX: Use char buffer for path building to avoid String allocs in recursion
static bool deletePathRecursiveInternal(const char* path, size_t pathLen, uint8_t depth) {
    if (depth > MAX_RECURSION_DEPTH) {
        Serial.printf("[FILESERVER] Delete depth limit exceeded at: %s\n", path);
        return false;  // Refuse to go deeper to protect stack
    }
    
    recursiveYieldCheck();  // FIX: Yield at start of each recursion level
    
    File f = SD.open(path);
    if (!f) {
        return false;
    }
    
    bool isDir = f.isDirectory();
    f.close();
    
    if (!isDir) {
        bool ok = SD.remove(path);
        return ok;
    }
    
    // It's a directory - delete all contents first (depth-first)
    File dir = SD.open(path);
    if (!dir) {
        return false;
    }
    
    // FIX: Stack buffer for child paths - sized for reasonable nesting
    char childPath[256];
    
    File entry = dir.openNextFile();
    while (entry) {
        const char* entryName = basenameFromPath(entry.name());
        size_t entryNameLen = strlen(entryName);
        
        // Build child path in stack buffer
        if (pathLen + 1 + entryNameLen >= sizeof(childPath)) {
            Serial.printf("[FILESERVER] Path too long in delete: %s/%s\n", path, entryName);
            entry.close();
            dir.close();
            return false;
        }
        memcpy(childPath, path, pathLen);
        childPath[pathLen] = '/';
        memcpy(childPath + pathLen + 1, entryName, entryNameLen + 1);  // +1 for null
        size_t childPathLen = pathLen + 1 + entryNameLen;
        
        bool entryIsDir = entry.isDirectory();
        entry.close();
        
        if (entryIsDir) {
            // Recurse into subdirectory
            if (!deletePathRecursiveInternal(childPath, childPathLen, depth + 1)) {
                dir.close();
                return false;
            }
        } else {
            if (!SD.remove(childPath)) {
                dir.close();
                return false;
            }
        }
        recursiveYieldCheck();  // FIX: More frequent yields during large deletes
        entry = dir.openNextFile();
    }
    dir.close();
    
    // Now remove the empty directory
    bool ok = SD.rmdir(path);
    return ok;
}

// Public wrapper - starts recursion at depth 0
bool FileServer::deletePathRecursive(const String& path) {
    recursiveOpLastYield = millis();  // FIX: Reset yield state for new operation
    recursiveOpCounter = 0;
    return deletePathRecursiveInternal(path.c_str(), path.length(), 0);
}

void FileServer::handleDelete() {
    String path = mapUiPathToFs(server->arg("f"));
    logRequest(server, "REQ");
    if (isTransferBusy()) {
        sendBusyResponse(server);
        return;
    }
    if (path.isEmpty()) {
        server->sendHeader("Connection", "close");
        server->send(400, "text/plain", "Missing path");
        return;
    }
    
    // Security: prevent directory traversal
    if (path.indexOf("..") >= 0) {
        server->sendHeader("Connection", "close");
        server->send(400, "text/plain", "Invalid path");
        return;
    }
    
    bool success = deletePathRecursive(path);
    
    if (success) {
        server->sendHeader("Connection", "close");
        server->send(200, "text/plain", "NUKED");
        } else {
        server->sendHeader("Connection", "close");
        server->send(500, "text/plain", "NUKE FAILED");
        }
}

void FileServer::handleBulkDelete() {
    logRequest(server, "REQ");
    if (isTransferBusy()) {
        sendBusyResponse(server);
        return;
    }
    // Read JSON body
    if (!server->hasArg("plain")) {
        server->sendHeader("Connection", "close");
        server->send(400, "application/json", "{\"error\":\"Missing body\"}");
        return;
    }
    
    String body = server->arg("plain");
    
    // FIX: Cap body size to prevent heap exhaustion
    if (body.length() > 4096) {
        server->sendHeader("Connection", "close");
        server->send(413, "application/json", "{\"error\":\"Body too large\"}");
        return;
    }
    
    // Simple JSON parsing for {"paths":["path1","path2",...]}
    // Using manual parsing to avoid ArduinoJson dependency in this module
    int deleted = 0;
    int failed = 0;
    
    int idx = body.indexOf("\"paths\"");
    if (idx < 0) {
        server->sendHeader("Connection", "close");
        server->send(400, "application/json", "{\"error\":\"Missing paths array\"}");
        return;
    }
    
    int arrStart = body.indexOf('[', idx);
    int arrEnd = body.indexOf(']', arrStart);
    if (arrStart < 0 || arrEnd < 0) {
        server->sendHeader("Connection", "close");
        server->send(400, "application/json", "{\"error\":\"Invalid paths array\"}");
        return;
    }
    
    String arrContent = body.substring(arrStart + 1, arrEnd);
    
    // Parse each path from the array
    int pos = 0;
    while (pos < (int)arrContent.length()) {
        int quoteStart = arrContent.indexOf('"', pos);
        if (quoteStart < 0) break;
        
        int quoteEnd = arrContent.indexOf('"', quoteStart + 1);
        if (quoteEnd < 0) break;
        
        String path = mapUiPathToFs(arrContent.substring(quoteStart + 1, quoteEnd));
        pos = quoteEnd + 1;
        
        // Security check
        if (path.indexOf("..") >= 0) {
            failed++;
            continue;
        }
        
        if (deletePathRecursive(path)) {
            deleted++;
            } else {
            failed++;
        }
        
        yield();  // Feed watchdog during bulk operations
    }
    
    // FIX: Use snprintf to avoid String temp allocations
    char response[64];
    snprintf(response, sizeof(response), "{\"deleted\":%d,\"failed\":%d}", deleted, failed);
    server->sendHeader("Connection", "close");
    server->send(200, "application/json", response);
}

void FileServer::handleMkdir() {
    String path = mapUiPathToFs(server->arg("f"));
    logRequest(server, "REQ");
    if (isTransferBusy()) {
        sendBusyResponse(server);
        return;
    }
    if (path.isEmpty()) {
        server->sendHeader("Connection", "close");
        server->send(400, "text/plain", "Missing path");
        return;
    }
    
    // Security: prevent directory traversal
    if (path.indexOf("..") >= 0) {
        server->sendHeader("Connection", "close");
        server->send(400, "text/plain", "Invalid path");
        return;
    }
    
    if (SD.mkdir(path)) {
        server->sendHeader("Connection", "close");
        server->send(200, "text/plain", "SPAWNED");
        } else {
        server->sendHeader("Connection", "close");
        server->send(500, "text/plain", "SPAWN FAILED");
        }
}

void FileServer::handleRename() {
    String oldPath = mapUiPathToFs(server->arg("old"));
    String newPath = mapUiPathToFs(server->arg("new"));
    logRequest(server, "REQ");
    if (isTransferBusy()) {
        sendBusyResponse(server);
        return;
    }
    
    if (oldPath.isEmpty() || newPath.isEmpty()) {
        server->sendHeader("Connection", "close");
        server->send(400, "application/json", "{\"success\":false,\"error\":\"Missing path\"}");
        return;
    }
    
    // Security: prevent directory traversal
    if (oldPath.indexOf("..") >= 0 || newPath.indexOf("..") >= 0) {
        server->sendHeader("Connection", "close");
        server->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid path\"}");
        return;
    }
    
    if (SD.rename(oldPath, newPath)) {
        server->sendHeader("Connection", "close");
        server->send(200, "application/json", "{\"success\":true}");
    } else {
        server->sendHeader("Connection", "close");
        server->send(500, "application/json", "{\"success\":false,\"error\":\"Rename failed\"}");
        }
}

bool FileServer::copyFileChunked(const String& srcPath, const String& dstPath) {
    // Copy a file in chunks to limit heap usage and avoid heap fragmentation.
    // Use a fixed static buffer to avoid repeated malloc/free cycles on the ESP32 heap.
    if (srcPath == dstPath) {
        return false;
    }

    File src = SD.open(srcPath, FILE_READ);
    if (!src) {
        return false;
    }

    File dst = SD.open(dstPath, FILE_WRITE);
    if (!dst) {
        src.close();
        return false;
    }

    const size_t COPY_CHUNK_SIZE = 4096;
    static uint8_t buf[COPY_CHUNK_SIZE];

    bool success = true;
    uint32_t lastYield = millis();
    uint32_t lastTimeout = millis();
    uint16_t yieldCounter = 0;
    
    while (src.available()) {
        size_t bytesToRead = COPY_CHUNK_SIZE;
        if (bytesToRead > src.available()) {
            bytesToRead = src.available();
        }
        size_t bytesRead = src.read(buf, bytesToRead);
        if (bytesRead == 0) {
            break;
        }
        if (dst.write(buf, bytesRead) != bytesRead) {
            success = false;
            break;
        }
        
        // Yield every 16 iterations (~64KB) to prevent WDT
        yieldCounter++;
        if (yieldCounter >= 16) {
            yield();
            yieldCounter = 0;
            
            // FIX: Check timeout BEFORE updating lastTimeout (was broken - always 0)
            uint32_t now = millis();
            if (now - lastTimeout > 30000) {
                Serial.println("[FILESERVER] Copy operation timed out");
                success = false;
                break;
            }
            lastTimeout = now;  // Update AFTER check
        }
    }

    src.close();
    dst.close();

    if (!success) {
        // Clean up partial copy on failure to avoid orphan files consuming space.
        SD.remove(dstPath);
    }
    return success;
}

// FIX: Added depth tracking to prevent stack overflow on deep directories
// FIX: Optimized to use char buffers for path building to reduce heap allocs in recursion
bool FileServer::copyPathRecursive(const String& srcPath, const String& dstPath, uint8_t depth) {
    if (depth > MAX_RECURSION_DEPTH) {
        Serial.printf("[FILESERVER] Copy depth limit exceeded at: %s\n", srcPath.c_str());
        return false;
    }
    
    recursiveYieldCheck();  // FIX: Yield at start of each recursion level
    
    File src = SD.open(srcPath);
    if (!src) {
        return false;
    }
    
    if (src.isDirectory()) {
        src.close();

        if (isSameOrSubPath(srcPath, dstPath)) {
            return false;
        }

        if (!SD.mkdir(dstPath)) {
            return false;
        }
        
        File dir = SD.open(srcPath);
        File entry;
        
        // FIX: Pre-cache path lengths and use stack buffers for child paths
        const char* srcBase = srcPath.c_str();
        const char* dstBase = dstPath.c_str();
        size_t srcLen = srcPath.length();
        size_t dstLen = dstPath.length();
        char newSrcBuf[256];
        char newDstBuf[256];
        
        while ((entry = dir.openNextFile())) {
            const char* fullName = entry.name();
            // Extract just filename from full path
            const char* name = basenameFromPath(fullName);
            size_t nameLen = strlen(name);
            
            // Build paths in stack buffers
            if (srcLen + 1 + nameLen >= sizeof(newSrcBuf) || 
                dstLen + 1 + nameLen >= sizeof(newDstBuf)) {
                Serial.printf("[FILESERVER] Path too long in copy: %s\n", name);
                entry.close();
                dir.close();
                return false;
            }
            
            memcpy(newSrcBuf, srcBase, srcLen);
            newSrcBuf[srcLen] = '/';
            memcpy(newSrcBuf + srcLen + 1, name, nameLen + 1);
            
            memcpy(newDstBuf, dstBase, dstLen);
            newDstBuf[dstLen] = '/';
            memcpy(newDstBuf + dstLen + 1, name, nameLen + 1);
            
            entry.close();
            
            // Create temporary String wrappers for recursive call (unavoidable for now)
            // but only one level of alloc instead of two + concat
            if (!copyPathRecursive(String(newSrcBuf), String(newDstBuf), depth + 1)) {
                dir.close();
                return false;
            }
            recursiveYieldCheck();  // FIX: More frequent yields during large copies
        }
        dir.close();
        return true;
    } else {
        src.close();
        return copyFileChunked(srcPath, dstPath);
    }
}

void FileServer::handleCopy() {
    logRequest(server, "REQ");
    if (isTransferBusy()) {
        sendBusyResponse(server);
        return;
    }
    if (!server->hasArg("plain")) {
        server->sendHeader("Connection", "close");
        server->send(400, "application/json", "{\"success\":false,\"error\":\"No body\"}");
        return;
    }
    
    String body = server->arg("plain");
    
    // FIX: Cap body size to prevent heap exhaustion
    if (body.length() > 4096) {
        server->sendHeader("Connection", "close");
        server->send(413, "application/json", "{\"success\":false,\"error\":\"Body too large\"}");
        return;
    }
    
    // Parse dest folder
    int destIdx = body.indexOf("\"dest\"");
    if (destIdx < 0) {
        server->sendHeader("Connection", "close");
        server->send(400, "application/json", "{\"success\":false,\"error\":\"Missing dest\"}");
        return;
    }
    int destStart = body.indexOf('"', body.indexOf(':', destIdx)) + 1;
    int destEnd = body.indexOf('"', destStart);
    String destDir = mapUiPathToFs(body.substring(destStart, destEnd));
    // Security check
    if (destDir.indexOf("..") >= 0) {
        server->sendHeader("Connection", "close");
        server->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid dest\"}");
        return;
    }
    
    // Parse files array
    int filesIdx = body.indexOf("\"files\"");
    if (filesIdx < 0) {
        server->sendHeader("Connection", "close");
        server->send(400, "application/json", "{\"success\":false,\"error\":\"Missing files\"}");
        return;
    }
    
    int arrStart = body.indexOf('[', filesIdx);
    int arrEnd = body.indexOf(']', arrStart);
    if (arrStart < 0 || arrEnd < 0) {
        server->sendHeader("Connection", "close");
        server->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid files array\"}");
        return;
    }
    
    String arrContent = body.substring(arrStart + 1, arrEnd);
    int copied = 0;
    int failed = 0;
    
    // FIX: Reset yield state before recursive operations
    recursiveOpLastYield = millis();
    recursiveOpCounter = 0;
    
    int pos = 0;
    while (pos < (int)arrContent.length()) {
        int quoteStart = arrContent.indexOf('"', pos);
        if (quoteStart < 0) break;
        
        int quoteEnd = arrContent.indexOf('"', quoteStart + 1);
        if (quoteEnd < 0) break;
        
        String srcPath = mapUiPathToFs(arrContent.substring(quoteStart + 1, quoteEnd));
        pos = quoteEnd + 1;
        
        if (srcPath.indexOf("..") >= 0) {
            failed++;
            continue;
        }
        
        // Extract filename from source path
        int lastSlash = srcPath.lastIndexOf('/');
        String filename = (lastSlash >= 0) ? srcPath.substring(lastSlash + 1) : srcPath;
        String dstPath = (destDir == "/") ? "/" + filename : destDir + "/" + filename;

        if (isSameOrSubPath(srcPath, dstPath)) {
            failed++;
            continue;
        }

        if (copyPathRecursive(srcPath, dstPath)) {
            copied++;
        } else {
            failed++;
        }
        
        recursiveYieldCheck();  // FIX: Yield between files in batch
    }
    
    // FIX: Use snprintf to avoid String temp allocations
    char response[80];
    snprintf(response, sizeof(response), "{\"success\":true,\"copied\":%d,\"failed\":%d}", copied, failed);
    server->sendHeader("Connection", "close");
    server->send(200, "application/json", response);
}

void FileServer::handleMove() {
    logRequest(server, "REQ");
    if (isTransferBusy()) {
        sendBusyResponse(server);
        return;
    }
    if (!server->hasArg("plain")) {
        server->sendHeader("Connection", "close");
        server->send(400, "application/json", "{\"success\":false,\"error\":\"No body\"}");
        return;
    }
    
    String body = server->arg("plain");
    
    // FIX: Cap body size to prevent heap exhaustion
    if (body.length() > 4096) {
        server->sendHeader("Connection", "close");
        server->send(413, "application/json", "{\"success\":false,\"error\":\"Body too large\"}");
        return;
    }
    
    // Parse dest folder
    int destIdx = body.indexOf("\"dest\"");
    if (destIdx < 0) {
        server->sendHeader("Connection", "close");
        server->send(400, "application/json", "{\"success\":false,\"error\":\"Missing dest\"}");
        return;
    }
    int destStart = body.indexOf('"', body.indexOf(':', destIdx)) + 1;
    int destEnd = body.indexOf('"', destStart);
    String destDir = mapUiPathToFs(body.substring(destStart, destEnd));
    if (destDir.indexOf("..") >= 0) {
        server->sendHeader("Connection", "close");
        server->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid dest\"}");
        return;
    }
    
    // Parse files array
    int filesIdx = body.indexOf("\"files\"");
    if (filesIdx < 0) {
        server->sendHeader("Connection", "close");
        server->send(400, "application/json", "{\"success\":false,\"error\":\"Missing files\"}");
        return;
    }
    
    int arrStart = body.indexOf('[', filesIdx);
    int arrEnd = body.indexOf(']', arrStart);
    if (arrStart < 0 || arrEnd < 0) {
        server->sendHeader("Connection", "close");
        server->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid files array\"}");
        return;
    }
    
    String arrContent = body.substring(arrStart + 1, arrEnd);
    int moved = 0;
    int failed = 0;
    
    // FIX: Reset yield state before recursive operations
    recursiveOpLastYield = millis();
    recursiveOpCounter = 0;
    
    int pos = 0;
    while (pos < (int)arrContent.length()) {
        int quoteStart = arrContent.indexOf('"', pos);
        if (quoteStart < 0) break;
        
        int quoteEnd = arrContent.indexOf('"', quoteStart + 1);
        if (quoteEnd < 0) break;
        
        String srcPath = mapUiPathToFs(arrContent.substring(quoteStart + 1, quoteEnd));
        pos = quoteEnd + 1;
        
        if (srcPath.indexOf("..") >= 0) {
            failed++;
            continue;
        }
        
        // Extract filename from source path
        int lastSlash = srcPath.lastIndexOf('/');
        String filename = (lastSlash >= 0) ? srcPath.substring(lastSlash + 1) : srcPath;
        String dstPath = (destDir == "/") ? "/" + filename : destDir + "/" + filename;

        if (isSameOrSubPath(srcPath, dstPath)) {
            failed++;
            continue;
        }

        // Try SD.rename first (fast, atomic)
        if (SD.rename(srcPath, dstPath)) {
            moved++;
        } else if (copyPathRecursive(srcPath, dstPath)) {
            // Fallback to copy+delete for cross-filesystem moves
            if (deletePathRecursive(srcPath)) {
                moved++;
            } else {
                // Rollback: delete the copy
                deletePathRecursive(dstPath);
                failed++;
            }
        } else {
            failed++;
        }
        
        recursiveYieldCheck();  // FIX: Yield between files in batch
    }
    
    // FIX: Use snprintf to avoid String temp allocations
    char response[80];
    snprintf(response, sizeof(response), "{\"success\":true,\"moved\":%d,\"failed\":%d}", moved, failed);
    server->sendHeader("Connection", "close");
    server->send(200, "application/json", response);
}

void FileServer::handleNotFound() {
    logRequest(server, "REQ");
    server->sendHeader("Connection", "close");
    server->send(404, "text/plain", "404: V01D");
}

const char* FileServer::getHTML() {
    return HTML_TEMPLATE;
}
