// GPS AT668 implementation

#include "gps.h"
#include "gps_quality.h"
#include "../core/config.h"
#include "../core/sdlog.h"
#include "../piglet/mood.h"
#include "../ui/display.h"
#include <sys/time.h>

// Static members
TinyGPSPlus GPS::gps;
HardwareSerial* GPS::serial = nullptr;
bool GPS::active = false;
GPSData GPS::currentData = {0};
uint32_t GPS::fixCount = 0;
uint32_t GPS::lastFixTime = 0;
uint32_t GPS::lastUpdateTime = 0;
SemaphoreHandle_t GPS::mutex = nullptr;
static bool gpsTimeSynced = false;

// ============================================================================
// NMEA Ring Buffer & Sentence Assembler
// ============================================================================

static constexpr uint16_t NMEA_BUF_SIZE = 640;   // One full NMEA burst + headroom
static constexpr uint8_t  SENTENCE_BUF_SIZE = 96; // Max NMEA sentence + slack

static char nmeaBuf[NMEA_BUF_SIZE];
static uint16_t nmeaHead = 0;  // Write position
static uint16_t nmeaTail = 0;  // Read position

static char sentenceBuf[SENTENCE_BUF_SIZE];
static uint8_t sentenceLen = 0;
static bool inSentence = false;

// ============================================================================
// GPS Statistics
// ============================================================================

static GPSStats stats = {0};

// ============================================================================
// Fix Coasting State
// ============================================================================

static GPSData coastSnapshot = {0};     // Last known good position
static uint32_t coastStartMs = 0;       // When coasting began
static bool coastActive = false;        // Currently coasting

// ============================================================================
// NMEA Helpers
// ============================================================================

// Compute XOR checksum over NMEA payload (between $ and *)
static bool validateNmeaChecksum(const char* sentence, uint8_t len) {
    // Minimum valid: $X*HH\r\n = 7 chars, but realistically longer
    if (len < 7) return false;
    if (sentence[0] != '$') return false;

    // Find the asterisk
    int starPos = -1;
    for (int i = 1; i < len; i++) {
        if (sentence[i] == '*') {
            starPos = i;
            break;
        }
    }
    if (starPos < 0 || starPos + 2 >= len) return false;

    // Compute XOR of bytes between $ and *
    uint8_t computed = 0;
    for (int i = 1; i < starPos; i++) {
        computed ^= (uint8_t)sentence[i];
    }

    // Parse hex checksum after *
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    int hi = hexVal(sentence[starPos + 1]);
    int lo = hexVal(sentence[starPos + 2]);
    if (hi < 0 || lo < 0) return false;

    uint8_t expected = (uint8_t)((hi << 4) | lo);
    return computed == expected;
}

// Ring buffer helpers
static inline uint16_t nmeaUsed() {
    return (nmeaHead >= nmeaTail)
        ? (nmeaHead - nmeaTail)
        : (NMEA_BUF_SIZE - nmeaTail + nmeaHead);
}

static inline uint16_t nmeaFree() {
    return NMEA_BUF_SIZE - 1 - nmeaUsed();
}

static inline char nmeaPeek(uint16_t offset) {
    return nmeaBuf[(nmeaTail + offset) % NMEA_BUF_SIZE];
}

static inline void nmeaAdvance(uint16_t count) {
    nmeaTail = (nmeaTail + count) % NMEA_BUF_SIZE;
}

// ============================================================================
// UART RX buffer size constant
// ============================================================================
static constexpr size_t UART_RX_BUF_SIZE = 1024;

void GPS::init(uint8_t rxPin, uint8_t txPin, uint32_t baud) {
    // GPS source now auto-configured via GPSSource enum in config
    // Pin selection happens in Config::load() based on gpsSource setting
    Serial.printf("[GPS] Init: RX=%d, TX=%d, baud=%lu\n", rxPin, txPin, baud);

    // Create mutex for thread safety
    if (mutex == nullptr) {
        mutex = xSemaphoreCreateMutex();
    }

    // Use Serial2 for GPS (UART2) — increase RX buffer before begin()
    Serial2.setRxBufferSize(UART_RX_BUF_SIZE);
    Serial2.begin(baud, SERIAL_8N1, rxPin, txPin);
    serial = &Serial2;
    active = true;

    // Reset ring buffer state
    nmeaHead = nmeaTail = 0;
    sentenceLen = 0;
    inSentence = false;
    memset(&stats, 0, sizeof(stats));
    coastActive = false;

    // Clear initial data - safe to use portMAX_DELAY during init (not a hot path, mutex just created)
    if (mutex && xSemaphoreTake(mutex, portMAX_DELAY)) {
        memset(&currentData, 0, sizeof(GPSData));
        currentData.valid = false;
        currentData.fix = false;
        currentData.coasting = false;
        xSemaphoreGive(mutex);
    }
}

void GPS::reinit(uint8_t rxPin, uint8_t txPin, uint32_t baud) {
    // Stop existing serial connection
    if (serial) {
        Serial2.end();
        serial = nullptr;
        active = false;
    }

    // Small delay to let hardware settle
    delay(50);

    // Re-initialize with new parameters — increase RX buffer before begin()
    Serial2.setRxBufferSize(UART_RX_BUF_SIZE);
    Serial2.begin(baud, SERIAL_8N1, rxPin, txPin);
    serial = &Serial2;
    active = true;

    // Reset ring buffer state
    nmeaHead = nmeaTail = 0;
    sentenceLen = 0;
    inSentence = false;
    coastActive = false;

    // Reset GPS state - safe to use portMAX_DELAY during reinit (configuration path, not hot path)
    if (mutex && xSemaphoreTake(mutex, portMAX_DELAY)) {
        memset(&currentData, 0, sizeof(GPSData));
        currentData.valid = false;
        currentData.fix = false;
        currentData.coasting = false;
        xSemaphoreGive(mutex);
    }
}

void GPS::update() {
    if (!active || serial == nullptr) return;

    processSerial();

    uint32_t now = millis();

    // Update data periodically
    if (now - lastUpdateTime > 100) {
        updateData();
        lastUpdateTime = now;
    }
}

void GPS::processSerial() {
    if (!serial) return;

    const uint32_t maxBytesPerCall = 128;
    uint32_t processedThisCall = 0;

    // Phase 1: Drain UART into ring buffer
    while (serial->available() > 0 && processedThisCall < maxBytesPerCall) {
        char c = serial->read();
        if (c == -1) continue;

        if (nmeaFree() > 0) {
            nmeaBuf[nmeaHead] = c;
            nmeaHead = (nmeaHead + 1) % NMEA_BUF_SIZE;
        } else {
            stats.rxOverflow++;
            stats.bytesDropped++;
        }

        processedThisCall++;
        if ((processedThisCall & 31) == 0) {
            yield();
        }
    }

    // Phase 2: Extract and validate sentences from ring buffer
    while (nmeaUsed() > 0) {
        char c = nmeaPeek(0);
        nmeaAdvance(1);

        if (c == '$') {
            // Start of new sentence — reset assembler
            sentenceBuf[0] = '$';
            sentenceLen = 1;
            inSentence = true;
            continue;
        }

        if (!inSentence) continue;

        if (c == '\n' || c == '\r') {
            if (sentenceLen < 6) {
                // Too short to be valid
                inSentence = false;
                sentenceLen = 0;
                continue;
            }

            // Terminate and validate
            sentenceBuf[sentenceLen] = '\0';

            if (validateNmeaChecksum(sentenceBuf, sentenceLen)) {
                // Feed valid sentence to TinyGPSPlus
                for (uint8_t i = 0; i < sentenceLen; i++) {
                    gps.encode(sentenceBuf[i]);
                }
                gps.encode('\r');
                gps.encode('\n');
                stats.validSentences++;
            } else {
                stats.failedChecksum++;
            }

            inSentence = false;
            sentenceLen = 0;
            continue;
        }

        // Accumulate character
        if (sentenceLen < SENTENCE_BUF_SIZE - 1) {
            sentenceBuf[sentenceLen++] = c;
        } else {
            // Overlong sentence — discard
            stats.overlong++;
            inSentence = false;
            sentenceLen = 0;
        }
    }
}

void GPS::updateData() {
    if (mutex == nullptr) return;  // FIX: Prevent crash if GPS not initialized

    // Get current GPS data safely
    bool valid = gps.location.isValid();
    double latitude = gps.location.lat();
    double longitude = gps.location.lng();
    double altitude = gps.altitude.meters();
    float speed = gps.speed.kmph();
    float course = gps.course.deg();
    uint8_t satellites = gps.satellites.value();
    uint16_t hdop = gps.hdop.value();
    uint32_t date = gps.date.isValid() ? gps.date.value() : 0;
    uint32_t time = gps.time.isValid() ? gps.time.value() : 0;
    uint32_t age = gps.location.age();

    // Quality-gated fix determination
    bool rawFix = valid && (age < GPSQuality::MAX_AGE_MS);
    bool qualityOk = GPSQuality::isFixAcceptable(satellites, hdop);
    bool fix = rawFix && qualityOk;

    if (rawFix && !qualityOk) {
        stats.qualityRejects++;
    }

    // Coasting logic: hold last known good position when fix drops
    bool coasting = false;
    uint32_t now = millis();

    if (fix) {
        // Active fix — update coast snapshot if quality is high enough
        if (GPSQuality::isCoastWorthy(satellites, hdop)) {
            coastSnapshot.latitude = latitude;
            coastSnapshot.longitude = longitude;
            coastSnapshot.altitude = altitude;
            coastSnapshot.speed = speed;
            coastSnapshot.course = course;
            coastSnapshot.satellites = satellites;
            coastSnapshot.hdop = hdop;
            coastSnapshot.date = date;
            coastSnapshot.time = time;
            coastSnapshot.valid = true;
            coastSnapshot.fix = true;
            coastSnapshot.coasting = false;
            coastSnapshot.age = age;
            coastStartMs = now;  // Reset coast timer on each good fix
        }
        if (coastActive) {
            coastActive = false;
        }
    } else if (coastSnapshot.valid && (now - coastStartMs) < GPSQuality::COAST_MS) {
        // Fix lost but within coast window — hold position
        if (!coastActive) {
            coastActive = true;
            stats.coastEvents++;
        }
        coasting = true;
        fix = true;  // Report as having fix (coasting)
        // Use frozen position from snapshot
        latitude = coastSnapshot.latitude;
        longitude = coastSnapshot.longitude;
        altitude = coastSnapshot.altitude;
        speed = coastSnapshot.speed;
        course = coastSnapshot.course;
        // Keep live satellite/HDOP so consumers see degradation
    } else {
        // Coast window expired or no snapshot
        if (coastActive) {
            coastActive = false;
            coastSnapshot.valid = false;
        }
    }

    // Update shared data atomically and check for fix changes - use timeout to prevent WDT
    bool hadFix = false;
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100))) {
        hadFix = currentData.fix;

        currentData.latitude = latitude;
        currentData.longitude = longitude;
        currentData.altitude = altitude;
        currentData.speed = speed;
        currentData.course = course;
        currentData.satellites = satellites;
        currentData.hdop = hdop;
        currentData.date = date;
        currentData.time = time;
        currentData.valid = valid;
        currentData.age = age;
        currentData.fix = fix;
        currentData.coasting = coasting;

        xSemaphoreGive(mutex);
    }

    // Process fix changes outside the mutex to avoid blocking
    if (fix && !hadFix) {
        // Increment fix count safely - use timeout to prevent WDT
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100))) {
            fixCount++;
            lastFixTime = millis();
            xSemaphoreGive(mutex);
        }
        stats.fixGainCount++;
        maybeSyncSystemTime(date, time);
        Mood::onGPSFix();
        Display::setGPSStatus(true);
        Serial.println("[GPS] Fix acquired!");
        SDLog::log("GPS", "Fix acquired (sats: %d, hdop: %d)", satellites, hdop);
    } else if (!fix && hadFix) {
        stats.fixLostCount++;
        Mood::onGPSLost();
        Display::setGPSStatus(false);
        Serial.println("[GPS] Fix lost");
        SDLog::log("GPS", "Fix lost");
    }
}

void GPS::sleep() {
    if (!active) return;
    if (!serial) return;  // Safety check

    // AT6668 (ATGM336H) does not support u-blox UBX protocol.
    // Stop UART to cease processing and reduce CPU overhead.
    Serial2.end();
    serial = nullptr;
    active = false;

    // Clear coasting state
    coastActive = false;
    coastSnapshot.valid = false;

    // Clear fix so GPS::hasFix() won't return stale true after sleep.
    // Without this, consumers (WARHOG etc.) would pick stale local GPS
    // data over fresh C5 GPS data because hasFix() gates the source selection.
    if (mutex && xSemaphoreTake(mutex, pdMS_TO_TICKS(100))) {
        currentData.fix = false;
        currentData.valid = false;
        currentData.coasting = false;
        xSemaphoreGive(mutex);
    }

    Display::setGPSStatus(false);
    Serial.println("[GPS] Entering sleep mode (UART stopped)");
}

void GPS::wake() {
    if (active) return;

    // Restart UART to resume GPS data processing.
    // AT6668 (ATGM336H) runs continuously — re-opening the port is sufficient.
    uint8_t rxPin = Config::gps().rxPin;
    uint8_t txPin = Config::gps().txPin;
    uint32_t baud = Config::gps().baudRate;
    Serial2.setRxBufferSize(UART_RX_BUF_SIZE);
    Serial2.begin(baud, SERIAL_8N1, rxPin, txPin);
    serial = &Serial2;
    active = true;

    // Reset ring buffer state
    nmeaHead = nmeaTail = 0;
    sentenceLen = 0;
    inSentence = false;

    Serial.println("[GPS] Waking up (UART restarted)");
}

void GPS::ensureContinuousMode() {
    // AT6668 (ATGM336H) runs continuously by default.
    // If UART was stopped (sleep), restart it. Otherwise just ensure flag is set.
    if (!serial) {
        uint8_t rxPin = Config::gps().rxPin;
        uint8_t txPin = Config::gps().txPin;
        uint32_t baud = Config::gps().baudRate;
        Serial2.setRxBufferSize(UART_RX_BUF_SIZE);
        Serial2.begin(baud, SERIAL_8N1, rxPin, txPin);
        serial = &Serial2;
    }
    active = true;
    Serial.println("[GPS] Continuous mode enforced");
}

void GPS::setPowerMode(bool isActive) {
    if (isActive) {
        wake();
    } else {
        sleep();
    }
}

bool GPS::isActive() {
    return active;
}

bool GPS::hasFix() {
    if (mutex == nullptr) return false;  // FIX: Prevent crash if GPS not initialized
    if (xSemaphoreTake(mutex, 10 / portTICK_PERIOD_MS)) {
        bool result = currentData.fix;
        xSemaphoreGive(mutex);
        return result;
    }
    return false; // Return safe value if mutex unavailable
}

GPSData GPS::getData() {
    GPSData data = {};
    if (mutex == nullptr) return data;  // FIX: Prevent crash if GPS not initialized
    if (xSemaphoreTake(mutex, 10 / portTICK_PERIOD_MS)) {
        data = currentData;
        xSemaphoreGive(mutex);
    }
    return data;
}

bool GPS::getLocationString(char* out, size_t len) {
    if (!out || len == 0) return false;
    if (mutex == nullptr) {  // FIX: Prevent crash if GPS not initialized
        strncpy(out, "No GPS", len - 1);
        out[len - 1] = '\0';
        return false;
    }
    if (xSemaphoreTake(mutex, 10 / portTICK_PERIOD_MS)) {
        if (currentData.fix) {
            int written = snprintf(out, len, "%.6f,%.6f",
                                   currentData.latitude, currentData.longitude);
            xSemaphoreGive(mutex);
            return (written > 0 && written < (int)len);
        } else {
            strncpy(out, "No fix", len - 1);
            out[len - 1] = '\0';
            xSemaphoreGive(mutex);
            return true;
        }
    } else {
        strncpy(out, "Error", len - 1);
        out[len - 1] = '\0';
        return false;
    }
}

void GPS::getTimeString(char* out, size_t len) {
    if (!out || len == 0) return;
    if (mutex == nullptr) {  // FIX: Prevent crash if GPS not initialized
        snprintf(out, len, "--:--");
        return;
    }
    if (xSemaphoreTake(mutex, 10 / portTICK_PERIOD_MS)) {
        if (gps.time.isValid()) {
            // Apply timezone offset from config
            int8_t tzOffset = Config::gps().timezoneOffset;
            int hour = gps.time.hour() + tzOffset;

            hour = ((hour % 24) + 24) % 24;

            snprintf(out, len, "%02d:%02d", hour, gps.time.minute());
        } else {
            snprintf(out, len, "--:--");
        }
        xSemaphoreGive(mutex);
    } else {
        snprintf(out, len, "ERR");
    }
}

uint32_t GPS::getFixCount() {
    if (mutex == nullptr) return 0;  // FIX: Prevent crash if GPS not initialized
    if (xSemaphoreTake(mutex, 10 / portTICK_PERIOD_MS)) {
        uint32_t count = fixCount;
        xSemaphoreGive(mutex);
        return count;
    }
    return 0; // Return safe value if mutex unavailable
}

uint32_t GPS::getLastFixTime() {
    if (mutex == nullptr) return 0;  // FIX: Prevent crash if GPS not initialized
    if (xSemaphoreTake(mutex, 10 / portTICK_PERIOD_MS)) {
        uint32_t time = lastFixTime;
        xSemaphoreGive(mutex);
        return time;
    }
    return 0; // Return safe value if mutex unavailable
}

GPSStats GPS::getStats() {
    return stats;  // All uint32_t fields, read atomically enough for diagnostics
}

bool GPS::isCoasting() {
    if (mutex == nullptr) return false;
    if (xSemaphoreTake(mutex, 10 / portTICK_PERIOD_MS)) {
        bool result = currentData.coasting;
        xSemaphoreGive(mutex);
        return result;
    }
    return false;
}

void GPS::maybeSyncSystemTime(uint32_t gpsDate, uint32_t gpsTime) {
    if (gpsTimeSynced) return;
    if (gpsDate == 0 || gpsTime == 0) return;

    // Skip if system time is already valid (NTP or PigSync ran first)
    time_t now = time(nullptr);
    if (now >= 1700000000) return;

    // Parse DDMMYY
    uint8_t day   = gpsDate / 10000;
    uint8_t month = (gpsDate / 100) % 100;
    uint16_t year = 2000 + (gpsDate % 100);

    // Parse HHMMSSCC
    uint8_t hour   = gpsTime / 1000000;
    uint8_t minute = (gpsTime / 10000) % 100;
    uint8_t second = (gpsTime / 100) % 100;

    // Sanity check
    if (year < 2024 || year > 2035) return;
    if (month < 1 || month > 12 || day < 1 || day > 31) return;
    if (hour > 23 || minute > 59 || second > 59) return;

    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min  = minute;
    t.tm_sec  = second;

    time_t epoch = mktime(&t);
    if (epoch < 1700000000) return;

    struct timeval tv;
    tv.tv_sec  = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    gpsTimeSynced = true;

    Serial.printf("[GPS] System time synced: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  year, month, day, hour, minute, second);
}
