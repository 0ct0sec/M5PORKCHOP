// GPS AT6668 Module Interface
#pragma once

#include <Arduino.h>
#include <TinyGPSPlus.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct GPSData {
    double latitude;
    double longitude;
    double altitude;
    float speed;
    float course;
    uint8_t satellites;
    uint16_t hdop;
    uint32_t date;
    uint32_t time;
    bool valid;
    bool fix;
    bool coasting;  // True when holding last known good position after fix loss
    uint32_t age;   // Age of last fix in ms
};

struct GPSStats {
    uint32_t validSentences;
    uint32_t failedChecksum;
    uint32_t rxOverflow;
    uint32_t overlong;
    uint32_t bytesDropped;
    uint32_t coastEvents;
    uint32_t qualityRejects;
    uint32_t fixLostCount;
    uint32_t fixGainCount;
};

class GPS {
public:
    static void init(uint8_t rxPin, uint8_t txPin, uint32_t baud = 9600);
    static void reinit(uint8_t rxPin, uint8_t txPin, uint32_t baud);  // Re-init with new pins
    static void update();
    static void sleep();
    static void wake();
    static void ensureContinuousMode();  // Force continuous mode regardless of software state
    
    static bool hasFix();
    static GPSData getData();
    static void getTimeString(char* out, size_t len);
    static bool getLocationString(char* out, size_t len);

    // System time sync from GPS (one-shot per boot, UTC)
    static void maybeSyncSystemTime(uint32_t gpsDate, uint32_t gpsTime);

    // Power management
    static void setPowerMode(bool active);
    static bool isActive();
    
    // Statistics
    static uint32_t getFixCount();
    static uint32_t getLastFixTime();
    static GPSStats getStats();
    static bool isCoasting();
    
private:
    static TinyGPSPlus gps;
    static HardwareSerial* serial;
    static bool active;
    static GPSData currentData;
    static uint32_t fixCount;
    static uint32_t lastFixTime;
    static uint32_t lastUpdateTime;
    static SemaphoreHandle_t mutex;
    
    static void processSerial();
    static void updateData();
};
