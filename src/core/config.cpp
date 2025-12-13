// Configuration management implementation

#include "config.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <SPIFFS.h>

// Static member initialization
GPSConfig Config::gpsConfig;
MLConfig Config::mlConfig;
WiFiConfig Config::wifiConfig;
BLEConfig Config::bleConfig;
PersonalityConfig Config::personalityConfig;
bool Config::initialized = false;
static bool sdAvailable = false;

bool Config::init() {
    // Initialize SPIFFS first (always available)
    if (!SPIFFS.begin(true)) {
        Serial.println("[CONFIG] SPIFFS mount failed");
    }
    
    // M5Cardputer handles SD initialization via M5.begin()
    // SD is on the built-in SD card slot (GPIO 12 for CS)
    
    if (!SD.begin(GPIO_NUM_12, SPI, 25000000)) {
        Serial.println("[CONFIG] SD card init failed, using SPIFFS");
        sdAvailable = false;
    } else {
        Serial.println("[CONFIG] SD card mounted");
        sdAvailable = true;
        
        // Create directories on SD if needed
        if (!SD.exists("/handshakes")) SD.mkdir("/handshakes");
        if (!SD.exists("/mldata")) SD.mkdir("/mldata");
        if (!SD.exists("/models")) SD.mkdir("/models");
        if (!SD.exists("/logs")) SD.mkdir("/logs");
        if (!SD.exists("/wardriving")) SD.mkdir("/wardriving");
    }
    
    // Load personality from SPIFFS (always use SPIFFS for settings)
    if (!loadPersonality()) {
        Serial.println("[CONFIG] Creating default personality");
        createDefaultPersonality();
        // Save defaults to SPIFFS
        savePersonalityToSPIFFS();
    }
    
    // Load main config
    if (!load()) {
        Serial.println("[CONFIG] Creating default config");
        createDefaultConfig();
    }
    
    initialized = true;
    return true;
}

bool Config::isSDAvailable() {
    return sdAvailable;
}

bool Config::load() {
    File file = SD.open(CONFIG_FILE, FILE_READ);
    if (!file) {
        Serial.println("[CONFIG] Cannot open config file");
        return false;
    }
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    
    if (err) {
        Serial.printf("[CONFIG] JSON parse error: %s\n", err.c_str());
        return false;
    }
    
    // GPS config
    if (doc["gps"].is<JsonObject>()) {
        gpsConfig.enabled = doc["gps"]["enabled"] | true;
        gpsConfig.rxPin = doc["gps"]["rxPin"] | 1;
        gpsConfig.txPin = doc["gps"]["txPin"] | 2;
        gpsConfig.baudRate = doc["gps"]["baudRate"] | 115200;
        gpsConfig.updateInterval = doc["gps"]["updateInterval"] | 5;
        gpsConfig.sleepTimeMs = doc["gps"]["sleepTimeMs"] | 5000;
        gpsConfig.powerSave = doc["gps"]["powerSave"] | true;
        gpsConfig.timezoneOffset = doc["gps"]["timezoneOffset"] | 0;
    }
    
    // ML config
    if (doc["ml"].is<JsonObject>()) {
        mlConfig.enabled = doc["ml"]["enabled"] | true;
        mlConfig.collectionMode = static_cast<MLCollectionMode>(doc["ml"]["collectionMode"] | 0);
        mlConfig.modelPath = doc["ml"]["modelPath"] | "/models/porkchop_model.bin";
        mlConfig.confidenceThreshold = doc["ml"]["confidenceThreshold"] | 0.7f;
        mlConfig.rogueApThreshold = doc["ml"]["rogueApThreshold"] | 0.8f;
        mlConfig.vulnScorerThreshold = doc["ml"]["vulnScorerThreshold"] | 0.6f;
        mlConfig.autoUpdate = doc["ml"]["autoUpdate"] | false;
        mlConfig.updateUrl = doc["ml"]["updateUrl"] | "";
    }
    
    // WiFi config
    if (doc["wifi"].is<JsonObject>()) {
        wifiConfig.channelHopInterval = doc["wifi"]["channelHopInterval"] | 500;
        wifiConfig.lockTime = doc["wifi"]["lockTime"] | 3000;
        wifiConfig.maxNetworks = doc["wifi"]["maxNetworks"] | 50;
        wifiConfig.enableDeauth = doc["wifi"]["enableDeauth"] | true;
        wifiConfig.randomizeMAC = doc["wifi"]["randomizeMAC"] | true;
        wifiConfig.otaSSID = doc["wifi"]["otaSSID"] | "";
        wifiConfig.otaPassword = doc["wifi"]["otaPassword"] | "";
        wifiConfig.autoConnect = doc["wifi"]["autoConnect"] | false;
    }
    
    // BLE config (PIGGY BLUES)
    if (doc["ble"].is<JsonObject>()) {
        bleConfig.burstInterval = doc["ble"]["burstInterval"] | 200;
        bleConfig.advDuration = doc["ble"]["advDuration"] | 100;
        bleConfig.scanDuration = doc["ble"]["scanDuration"] | 3000;
        bleConfig.rescanInterval = doc["ble"]["rescanInterval"] | 60;
    }
    
    Serial.println("[CONFIG] Loaded successfully");
    return true;
}

bool Config::loadPersonality() {
    // Load from SPIFFS (always available)
    File file = SPIFFS.open(PERSONALITY_FILE, FILE_READ);
    if (!file) {
        Serial.println("[CONFIG] Personality file not found in SPIFFS");
        return false;
    }
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    
    if (err) {
        Serial.printf("[CONFIG] Personality JSON error: %s\n", err.c_str());
        return false;
    }
    
    const char* name = doc["name"] | "Porkchop";
    strncpy(personalityConfig.name, name, sizeof(personalityConfig.name) - 1);
    personalityConfig.name[sizeof(personalityConfig.name) - 1] = '\0';
    
    personalityConfig.mood = doc["mood"] | 50;
    personalityConfig.experience = doc["experience"] | 0;
    personalityConfig.curiosity = doc["curiosity"] | 0.7f;
    personalityConfig.aggression = doc["aggression"] | 0.3f;
    personalityConfig.patience = doc["patience"] | 0.5f;
    personalityConfig.soundEnabled = doc["soundEnabled"] | true;
    personalityConfig.brightness = doc["brightness"] | 80;
    personalityConfig.dimLevel = doc["dimLevel"] | 20;
    personalityConfig.dimTimeout = doc["dimTimeout"] | 30;
    
    Serial.printf("[CONFIG] Personality: %s (mood: %d, sound: %s, bright: %d%%, dim: %ds)\n", 
                  personalityConfig.name, 
                  personalityConfig.mood,
                  personalityConfig.soundEnabled ? "ON" : "OFF",
                  personalityConfig.brightness,
                  personalityConfig.dimTimeout);
    return true;
}

void Config::savePersonalityToSPIFFS() {
    JsonDocument doc;
    doc["name"] = personalityConfig.name;
    doc["mood"] = personalityConfig.mood;
    doc["experience"] = personalityConfig.experience;
    doc["curiosity"] = personalityConfig.curiosity;
    doc["aggression"] = personalityConfig.aggression;
    doc["patience"] = personalityConfig.patience;
    doc["soundEnabled"] = personalityConfig.soundEnabled;
    doc["brightness"] = personalityConfig.brightness;
    doc["dimLevel"] = personalityConfig.dimLevel;
    doc["dimTimeout"] = personalityConfig.dimTimeout;
    
    File file = SPIFFS.open(PERSONALITY_FILE, FILE_WRITE);
    if (file) {
        serializeJsonPretty(doc, file);
        file.close();
        Serial.printf("[CONFIG] Saved personality to SPIFFS (sound: %s)\n",
                     personalityConfig.soundEnabled ? "ON" : "OFF");
    } else {
        Serial.println("[CONFIG] Failed to save personality to SPIFFS");
    }
}

bool Config::save() {
    JsonDocument doc;
    
    // GPS config
    doc["gps"]["enabled"] = gpsConfig.enabled;
    doc["gps"]["rxPin"] = gpsConfig.rxPin;
    doc["gps"]["txPin"] = gpsConfig.txPin;
    doc["gps"]["baudRate"] = gpsConfig.baudRate;
    doc["gps"]["updateInterval"] = gpsConfig.updateInterval;
    doc["gps"]["sleepTimeMs"] = gpsConfig.sleepTimeMs;
    doc["gps"]["powerSave"] = gpsConfig.powerSave;
    doc["gps"]["timezoneOffset"] = gpsConfig.timezoneOffset;
    
    // ML config
    doc["ml"]["enabled"] = mlConfig.enabled;
    doc["ml"]["collectionMode"] = static_cast<uint8_t>(mlConfig.collectionMode);
    doc["ml"]["modelPath"] = mlConfig.modelPath;
    doc["ml"]["confidenceThreshold"] = mlConfig.confidenceThreshold;
    doc["ml"]["rogueApThreshold"] = mlConfig.rogueApThreshold;
    doc["ml"]["vulnScorerThreshold"] = mlConfig.vulnScorerThreshold;
    doc["ml"]["autoUpdate"] = mlConfig.autoUpdate;
    doc["ml"]["updateUrl"] = mlConfig.updateUrl;
    
    // WiFi config
    doc["wifi"]["channelHopInterval"] = wifiConfig.channelHopInterval;
    doc["wifi"]["lockTime"] = wifiConfig.lockTime;
    doc["wifi"]["maxNetworks"] = wifiConfig.maxNetworks;
    doc["wifi"]["enableDeauth"] = wifiConfig.enableDeauth;
    doc["wifi"]["randomizeMAC"] = wifiConfig.randomizeMAC;
    doc["wifi"]["otaSSID"] = wifiConfig.otaSSID;
    doc["wifi"]["otaPassword"] = wifiConfig.otaPassword;
    doc["wifi"]["autoConnect"] = wifiConfig.autoConnect;
    
    // BLE config (PIGGY BLUES)
    doc["ble"]["burstInterval"] = bleConfig.burstInterval;
    doc["ble"]["advDuration"] = bleConfig.advDuration;
    doc["ble"]["scanDuration"] = bleConfig.scanDuration;
    doc["ble"]["rescanInterval"] = bleConfig.rescanInterval;
    
    File file = SD.open(CONFIG_FILE, FILE_WRITE);
    if (!file) {
        return false;
    }
    
    serializeJsonPretty(doc, file);
    file.close();
    return true;
}

bool Config::createDefaultConfig() {
    gpsConfig = GPSConfig();
    mlConfig = MLConfig();
    wifiConfig = WiFiConfig();
    bleConfig = BLEConfig();
    return true;
}

bool Config::createDefaultPersonality() {
    strcpy(personalityConfig.name, "Porkchop");
    personalityConfig.mood = 50;
    personalityConfig.experience = 0;
    personalityConfig.curiosity = 0.7f;
    personalityConfig.aggression = 0.3f;
    personalityConfig.patience = 0.5f;
    personalityConfig.soundEnabled = true;
    return true;
}

void Config::setGPS(const GPSConfig& cfg) {
    gpsConfig = cfg;
    save();
}

void Config::setML(const MLConfig& cfg) {
    mlConfig = cfg;
    save();
}

void Config::setWiFi(const WiFiConfig& cfg) {
    wifiConfig = cfg;
    save();
}

void Config::setBLE(const BLEConfig& cfg) {
    bleConfig = cfg;
    save();
}

void Config::setPersonality(const PersonalityConfig& cfg) {
    personalityConfig = cfg;
    
    // Save personality to SPIFFS (always available)
    savePersonalityToSPIFFS();
}
