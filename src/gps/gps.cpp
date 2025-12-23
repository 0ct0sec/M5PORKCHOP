// GPS AT6668 implementation

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

void GPS::init(uint8_t rxPin, uint8_t txPin, uint32_t baud) {
    // Use Serial2 for GPS (UART2)
    Serial2.begin(baud, SERIAL_8N1, rxPin, txPin);
    serial = &Serial2;
    active = true;
    
    // Clear initial data
    memset(&currentData, 0, sizeof(GPSData));
    currentData.valid = false;
    currentData.fix = false;
    
    // GPS logs silenced - pig prefers stealth
    // Serial.printf("[GPS] Initialized on pins RX:%d TX:%d @ %d baud\n", rxPin, txPin, baud);
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
    
    // Reset GPS state
    memset(&currentData, 0, sizeof(GPSData));
    currentData.valid = false;
    currentData.fix = false;
    
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
    static uint32_t bytesReceived = 0;
    
    while (serial->available() > 0) {
        char c = serial->read();
        gps.encode(c);
        bytesReceived++;
    }
    
    // GPS debug logs silenced - pig prefers stealth
    // Uncomment for debugging:
    // uint32_t now = millis();
    // if (now - lastDebugTime >= 5000) {
    //     Serial.printf("[GPS] Bytes: %lu, Sats: %d, Valid: %s\n", bytesReceived, gps.satellites.value(), gps.location.isValid() ? "Y" : "N");
    //     lastDebugTime = now;
    // }
}

void GPS::updateData() {
    bool hadFix = currentData.fix;
    
    currentData.latitude = gps.location.lat();
    currentData.longitude = gps.location.lng();
    currentData.altitude = gps.altitude.meters();
    currentData.speed = gps.speed.kmph();
    currentData.course = gps.course.deg();
    currentData.satellites = gps.satellites.value();
    currentData.hdop = gps.hdop.value();
    
    // Date and time
    if (gps.date.isValid()) {
        currentData.date = gps.date.value();  // DDMMYY
    }
    if (gps.time.isValid()) {
        currentData.time = gps.time.value();  // HHMMSSCC
    }
    
    // Fix status
    currentData.valid = gps.location.isValid();
    currentData.age = gps.location.age();
    // Relaxed age check: 30 seconds for wardriving (GPS may not update every sentence)
    // Also accept if we have valid coords even with stale age (indoor/tunnel resume)
    currentData.fix = currentData.valid && (currentData.age < 30000);
    
    // Track fix changes
    if (currentData.fix && !hadFix) {
        fixCount++;
        lastFixTime = millis();
        Mood::onGPSFix();
        Display::setGPSStatus(true);
        Serial.println("[GPS] Fix acquired!");
        SDLog::log("GPS", "Fix acquired (sats: %d)", currentData.satellites);
    } else if (!currentData.fix && hadFix) {
        Mood::onGPSLost();
        Display::setGPSStatus(false);
        Serial.println("[GPS] Fix lost");
        SDLog::log("GPS", "Fix lost");
    }
}

void GPS::sleep() {
    if (!active) return;
    if (!serial) return;  // Safety check
    
    // Send sleep command to AT6668 (UBX protocol)
    // CFG-RXM - Power Save Mode
    uint8_t sleepCmd[] = {
        0xB5, 0x62,  // Sync
        0x06, 0x11,  // CFG-RXM
        0x02, 0x00,  // Length
        0x08, 0x01,  // reserved, Power Save Mode
        0x22, 0x92   // Checksum
    };
    
    serial->write(sleepCmd, sizeof(sleepCmd));
    active = false;
    Serial.println("[GPS] Entering sleep mode");
}

void GPS::wake() {
    if (active) return;
    if (!serial) return;  // Safety check
    
    // Send wake command
    uint8_t wakeCmd[] = {
        0xB5, 0x62,  // Sync
        0x06, 0x11,  // CFG-RXM
        0x02, 0x00,  // Length
        0x08, 0x00,  // reserved, Continuous Mode
        0x21, 0x91   // Checksum
    };
    
    serial->write(wakeCmd, sizeof(wakeCmd));
    active = true;
    Serial.println("[GPS] Waking up");
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
    return currentData.fix;
}

GPSData GPS::getData() {
    return currentData;
}

String GPS::getLocationString() {
    if (!currentData.fix) {
        return "No fix";
    }
    
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6f,%.6f", 
             currentData.latitude, currentData.longitude);
    return String(buf);
}

String GPS::getTimeString() {
    if (!gps.time.isValid()) {
        return "--:--";
    }
    
    // Apply timezone offset from config
    int8_t tzOffset = Config::gps().timezoneOffset;
    int hour = gps.time.hour() + tzOffset;
    
    // Handle day wrap
    if (hour >= 24) hour -= 24;
    if (hour < 0) hour += 24;
    
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", hour, gps.time.minute());
    return String(buf);
}
