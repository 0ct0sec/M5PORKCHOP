// GPS AT668 implementation

#include "gps.h"
#include "../core/config.h"
#include "../core/sdlog.h"
#include "../piglet/mood.h"
#include "../ui/display.h"

// Static members
TinyGPSPlus GPS::gps;
HardwareSerial* GPS::serial = nullptr;
bool GPS::active = false;
GPSData GPS::currentData = {0};
uint32_t GPS::fixCount = 0;
uint32_t GPS::lastFixTime = 0;
uint32_t GPS::lastUpdateTime = 0;
SemaphoreHandle_t GPS::mutex = nullptr;

void GPS::init(uint8_t rxPin, uint8_t txPin, uint32_t baud) {
    // GPS source now auto-configured via GPSSource enum in config
    // Pin selection happens in Config::load() based on gpsSource setting
    Serial.printf("[GPS] Init: RX=%d, TX=%d, baud=%lu\n", rxPin, txPin, baud);
    
    // Create mutex for thread safety
    if (mutex == nullptr) {
        mutex = xSemaphoreCreateMutex();
    }
    
    // Use Serial2 for GPS (UART2)
    Serial2.begin(baud, SERIAL_8N1, rxPin, txPin);
    serial = &Serial2;
    active = true;
    
    // Clear initial data - safe to use portMAX_DELAY during init (not a hot path, mutex just created)
    if (mutex && xSemaphoreTake(mutex, portMAX_DELAY)) {
        memset(&currentData, 0, sizeof(GPSData));
        currentData.valid = false;
        currentData.fix = false;
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
    
    // Re-initialize with new parameters
    Serial2.begin(baud, SERIAL_8N1, rxPin, txPin);
    serial = &Serial2;
    active = true;
    
    // Reset GPS state - safe to use portMAX_DELAY during reinit (configuration path, not hot path)
    if (mutex && xSemaphoreTake(mutex, portMAX_DELAY)) {
        memset(&currentData, 0, sizeof(GPSData));
        currentData.valid = false;
        currentData.fix = false;
        xSemaphoreGive(mutex);
    }
    
    // GPS logs silenced - pig prefers stealth
    // Serial.printf("[GPS] Re-initialized on pins RX:%d TX:%d @ %d baud\n", rxPin, txPin, baud);
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
    if (!serial) return;  // Safety check
    
    static uint32_t lastDebugTime = 0;
    static uint32_t bytesProcessed = 0;
    uint32_t processedThisCall = 0;
    const uint32_t maxBytesPerCall = 128; // Limit processing per call to prevent WDT
    
    while (serial->available() > 0 && processedThisCall < maxBytesPerCall) {
        char c = serial->read();
        if (c != -1) { // Valid byte read
            gps.encode(c);
            bytesProcessed++;
            processedThisCall++;
        }
    }
    
    // Yield occasionally during heavy processing to prevent WDT
    if (processedThisCall > 0 && processedThisCall % 32 == 0) {
        yield(); // Allow other tasks to run
    }
    
    // GPS debug logs silenced - pig prefers stealth
    // Uncomment for debugging:
    // uint32_t now = millis();
    // if (now - lastDebugTime >= 5000) {
    //     Serial.printf("[GPS] Bytes: %lu, Sats: %d, Valid: %s\n", bytesProcessed, gps.satellites.value(), gps.location.isValid() ? "Y" : "N");
    //     lastDebugTime = now;
    // }
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
    bool fix = valid && (age < 30000);
    
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
        Mood::onGPSFix();
        Display::setGPSStatus(true);
        Serial.println("[GPS] Fix acquired!");
        SDLog::log("GPS", "Fix acquired (sats: %d)", satellites);
    } else if (!fix && hadFix) {
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
    Serial.println("[GPS] Entering sleep mode (UART stopped)");
}

void GPS::wake() {
    if (active) return;

    // Restart UART to resume GPS data processing.
    // AT6668 (ATGM336H) runs continuously — re-opening the port is sufficient.
    uint8_t rxPin = Config::gps().rxPin;
    uint8_t txPin = Config::gps().txPin;
    uint32_t baud = Config::gps().baudRate;
    Serial2.begin(baud, SERIAL_8N1, rxPin, txPin);
    serial = &Serial2;
    active = true;
    Serial.println("[GPS] Waking up (UART restarted)");
}

void GPS::ensureContinuousMode() {
    // AT6668 (ATGM336H) runs continuously by default.
    // If UART was stopped (sleep), restart it. Otherwise just ensure flag is set.
    if (!serial) {
        uint8_t rxPin = Config::gps().rxPin;
        uint8_t txPin = Config::gps().txPin;
        uint32_t baud = Config::gps().baudRate;
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
            hour = ((hour % 24) + 24) % 24;  // handles any offset range, not just ±1 day
            
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
