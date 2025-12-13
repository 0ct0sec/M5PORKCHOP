// Configuration management
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#define CONFIG_FILE "/porkchop.conf"
#define PERSONALITY_FILE "/personality.json"

// GPS power management settings
struct GPSConfig {
    bool enabled = true;
    uint8_t rxPin = 1;              // G1 for Grove GPS, G13 for Cap LoRa868
    uint8_t txPin = 2;              // G2 for Grove GPS, G15 for Cap LoRa868
    uint32_t baudRate = 115200;     // 115200 for most modern GPS modules
    uint16_t updateInterval = 5;        // Seconds between GPS updates
    uint16_t sleepTimeMs = 5000;        // Sleep duration when stationary
    bool powerSave = true;
    int8_t timezoneOffset = 0;          // Hours offset from UTC (-12 to +14)
};

// ML data collection mode
enum class MLCollectionMode : uint8_t {
    BASIC = 0,      // Use ESP-IDF scan API only (faster, less features)
    ENHANCED = 1    // Use promiscuous beacon capture (slower, full features)
};

// ML settings
struct MLConfig {
    bool enabled = true;
    MLCollectionMode collectionMode = MLCollectionMode::BASIC;  // Data collection mode
    String modelPath = "/models/porkchop_model.bin";
    float confidenceThreshold = 0.7f;
    float rogueApThreshold = 0.8f;
    float vulnScorerThreshold = 0.6f;
    bool autoUpdate = false;
    String updateUrl = "";
};

// WiFi settings for scanning and OTA
struct WiFiConfig {
    uint16_t channelHopInterval = 500;
    uint16_t lockTime = 3000;           // Time to discover clients before attacking (ms)
    uint16_t maxNetworks = 50;
    bool enableDeauth = true;
    bool randomizeMAC = true;           // Randomize MAC on mode start for stealth
    String otaSSID = "";
    String otaPassword = "";
    bool autoConnect = false;
};

// BLE settings for PIGGY BLUES mode
struct BLEConfig {
    uint16_t burstInterval = 200;       // ms between advertisement bursts (50-500)
    uint16_t advDuration = 100;         // ms per advertisement (50-200)
    uint16_t scanDuration = 3000;       // Device scan duration (1000-10000)
    uint16_t rescanInterval = 60;       // Seconds between rescans (30-120)
};

// Personality settings
struct PersonalityConfig {
    char name[32] = "Porkchop";
    int mood = 50;                      // -100 to 100
    uint32_t experience = 0;
    float curiosity = 0.7f;
    float aggression = 0.3f;
    float patience = 0.5f;
    bool soundEnabled = true;
    uint8_t brightness = 80;            // Display brightness 0-100%
    uint8_t dimLevel = 20;              // Dimmed brightness 0-100% (0 = off)
    uint16_t dimTimeout = 30;           // Seconds before dimming (0 = never)
};

class Config {
public:
    static bool init();
    static bool save();
    static bool load();
    static bool loadPersonality();
    static bool isSDAvailable();
    
    // Getters
    static GPSConfig& gps() { return gpsConfig; }
    static MLConfig& ml() { return mlConfig; }
    static WiFiConfig& wifi() { return wifiConfig; }
    static BLEConfig& ble() { return bleConfig; }
    static PersonalityConfig& personality() { return personalityConfig; }
    
    // Setters with auto-save
    static void setGPS(const GPSConfig& cfg);
    static void setML(const MLConfig& cfg);
    static void setWiFi(const WiFiConfig& cfg);
    static void setBLE(const BLEConfig& cfg);
    static void setPersonality(const PersonalityConfig& cfg);
    
private:
    static GPSConfig gpsConfig;
    static MLConfig mlConfig;
    static WiFiConfig wifiConfig;
    static BLEConfig bleConfig;
    static PersonalityConfig personalityConfig;
    static bool initialized;
    
    static bool createDefaultConfig();
    static bool createDefaultPersonality();
    static void savePersonalityToSPIFFS();
};
