// Configuration management implementation

#include "config.h"
#include "sdlog.h"
#include <M5Cardputer.h>
#include <SD.h>
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
    
    // Allow SPI bus to stabilize after M5.begin()
    // Critical for Cardputer ADV and v1.1 which share SPI with display
    delay(50);
    
    // M5Cardputer handles SD initialization via M5.begin()
    // SD is on the built-in SD card slot (GPIO 12 for CS)
    // Retry with progressive SPI speeds for reliability
    sdAvailable = false;
    const int maxRetries = 3;
    const uint32_t speeds[] = {10000000, 20000000, 25000000};  // 10MHz, 20MHz, 25MHz
    
    for (int attempt = 0; attempt < maxRetries && !sdAvailable; attempt++) {
        uint32_t speed = speeds[attempt];
        Serial.printf("[CONFIG] SD init attempt %d/%d at %luMHz\n", 
                      attempt + 1, maxRetries, speed / 1000000);
        
        if (attempt > 0) {
            SD.end();  // Clean up previous failed attempt
            delay(100);  // Allow bus to settle
        }
        
        if (SD.begin(GPIO_NUM_12, SPI, speed)) {
            Serial.printf("[CONFIG] SD card mounted at %luMHz\n", speed / 1000000);
            sdAvailable = true;
        }
    }
    
    if (!sdAvailable) {
        Serial.println("[CONFIG] SD card init failed after retries, using SPIFFS");
    } else {
        SDLog::log("CFG", "SD card mounted OK");
        
        // Create directories on SD if needed
        if (!SD.exists("/M5PORKCHOP/handshakes")) SD.mkdir("/M5PORKCHOP/handshakes");
        if (!SD.exists("/M5PORKCHOP/mldata")) SD.mkdir("/M5PORKCHOP/mldata");
        if (!SD.exists("/M5PORKCHOP/models")) SD.mkdir("/M5PORKCHOP/models");
        if (!SD.exists("/M5PORKCHOP/logs")) SD.mkdir("/M5PORKCHOP/logs");
        if (!SD.exists("/M5PORKCHOP/wardriving")) SD.mkdir("/M5PORKCHOP/wardriving");
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
    
    // Try to load WPA-SEC key from file (auto-deletes after import)
    if (loadWpaSecKeyFromFile()) {
        Serial.println("[CONFIG] WPA-SEC key loaded from file");
    }
    
    initialized = true;
    return true;
}

bool Config::isSDAvailable() {
    return sdAvailable;
}

bool Config::reinitSD() {
    Serial.println("[CONFIG] Attempting SD card re-initialization...");
    
    // Clean up any existing SD state
    SD.end();
    delay(100);
    
    // Retry with progressive SPI speeds (same as init)
    sdAvailable = false;
    const int maxRetries = 3;
    const uint32_t speeds[] = {10000000, 20000000, 25000000};
    
    for (int attempt = 0; attempt < maxRetries && !sdAvailable; attempt++) {
        uint32_t speed = speeds[attempt];
        Serial.printf("[CONFIG] SD reinit attempt %d/%d at %luMHz\n", 
                      attempt + 1, maxRetries, speed / 1000000);
        
        if (attempt > 0) {
            SD.end();
            delay(100);
        }
        
        if (SD.begin(GPIO_NUM_12, SPI, speed)) {
            Serial.printf("[CONFIG] SD card mounted at %luMHz\n", speed / 1000000);
            sdAvailable = true;
        }
    }
    
    if (sdAvailable) {
        SDLog::log("CFG", "SD card re-initialized OK");
        
        // Ensure directories exist
        if (!SD.exists("/M5PORKCHOP/handshakes")) SD.mkdir("/M5PORKCHOP/handshakes");
        if (!SD.exists("/M5PORKCHOP/mldata")) SD.mkdir("/M5PORKCHOP/mldata");
        if (!SD.exists("/M5PORKCHOP/models")) SD.mkdir("/M5PORKCHOP/models");
        if (!SD.exists("/M5PORKCHOP/logs")) SD.mkdir("/M5PORKCHOP/logs");
        if (!SD.exists("/M5PORKCHOP/wardriving")) SD.mkdir("/M5PORKCHOP/wardriving");
    } else {
        Serial.println("[CONFIG] SD card reinit failed");
    }
    
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
        gpsConfig.source = static_cast<GPSSource>(doc["gps"]["gpsSource"] | 0);
        // Auto-set pins based on source (ignore legacy rxPin/txPin from config)
        if (gpsConfig.source == GPSSource::CAP_LORA) {
            gpsConfig.rxPin = 15;  // Cap LoRa868 GPS RX
            gpsConfig.txPin = 13;  // Cap LoRa868 GPS TX
        } else {
            gpsConfig.rxPin = 1;   // Grove GPS RX (default)
            gpsConfig.txPin = 2;   // Grove GPS TX (default)
        }
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
        mlConfig.modelPath = doc["ml"]["modelPath"] | "/M5PORKCHOP/models/porkchop_model.bin";
        mlConfig.confidenceThreshold = doc["ml"]["confidenceThreshold"] | 0.7f;
        mlConfig.rogueApThreshold = doc["ml"]["rogueApThreshold"] | 0.8f;
        mlConfig.vulnScorerThreshold = doc["ml"]["vulnScorerThreshold"] | 0.6f;
        mlConfig.autoUpdate = doc["ml"]["autoUpdate"] | false;
        mlConfig.updateUrl = doc["ml"]["updateUrl"] | "";
    }
    
    // WiFi config
    if (doc["wifi"].is<JsonObject>()) {
        wifiConfig.channelHopInterval = doc["wifi"]["channelHopInterval"] | 500;
        wifiConfig.lockTime = doc["wifi"]["lockTime"] | 12000;
        wifiConfig.enableDeauth = doc["wifi"]["enableDeauth"] | true;
        wifiConfig.randomizeMAC = doc["wifi"]["randomizeMAC"] | true;
        wifiConfig.otaSSID = doc["wifi"]["otaSSID"] | "";
        wifiConfig.otaPassword = doc["wifi"]["otaPassword"] | "";
        wifiConfig.autoConnect = doc["wifi"]["autoConnect"] | false;
        wifiConfig.wpaSecKey = doc["wifi"]["wpaSecKey"] | "";
        wifiConfig.wigleApiName = doc["wifi"]["wigleApiName"] | "";
        wifiConfig.wigleApiToken = doc["wifi"]["wigleApiToken"] | "";
    }
    
    // BLE config (PIGGY BLUES)
    if (doc["ble"].is<JsonObject>()) {
        bleConfig.burstInterval = doc["ble"]["burstInterval"] | 200;
        bleConfig.advDuration = doc["ble"]["advDuration"] | 100;
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
    personalityConfig.themeIndex = doc["themeIndex"] | 0;
    
    Serial.printf("[CONFIG] Personality: %s (mood: %d, sound: %s, bright: %d%%, dim: %ds, theme: %d)\n", 
                  personalityConfig.name, 
                  personalityConfig.mood,
                  personalityConfig.soundEnabled ? "ON" : "OFF",
                  personalityConfig.brightness,
                  personalityConfig.dimTimeout,
                  personalityConfig.themeIndex);
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
    doc["themeIndex"] = personalityConfig.themeIndex;
    
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
    doc["gps"]["gpsSource"] = static_cast<uint8_t>(gpsConfig.source);
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
    doc["wifi"]["enableDeauth"] = wifiConfig.enableDeauth;
    doc["wifi"]["randomizeMAC"] = wifiConfig.randomizeMAC;
    doc["wifi"]["otaSSID"] = wifiConfig.otaSSID;
    doc["wifi"]["otaPassword"] = wifiConfig.otaPassword;
    doc["wifi"]["autoConnect"] = wifiConfig.autoConnect;
    doc["wifi"]["wpaSecKey"] = wifiConfig.wpaSecKey;
    doc["wifi"]["wigleApiName"] = wifiConfig.wigleApiName;
    doc["wifi"]["wigleApiToken"] = wifiConfig.wigleApiToken;
    
    // BLE config (PIGGY BLUES)
    doc["ble"]["burstInterval"] = bleConfig.burstInterval;
    doc["ble"]["advDuration"] = bleConfig.advDuration;
    
    File file = SD.open(CONFIG_FILE, FILE_WRITE);
    if (!file) {
        return false;
    }
    
    size_t written = serializeJsonPretty(doc, file);
    file.close();
    
    // Check if write succeeded (serializeJson returns 0 on failure)
    return written > 0;
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

bool Config::loadWpaSecKeyFromFile() {
    const char* keyFile = "/wpasec_key.txt";
    
    if (!sdAvailable || !SD.exists(keyFile)) {
        return false;
    }
    
    File f = SD.open(keyFile, FILE_READ);
    if (!f) {
        Serial.println("[CONFIG] Failed to open wpasec_key.txt");
        return false;
    }
    
    // Read the key (first line, trim whitespace)
    String key = f.readStringUntil('\n');
    key.trim();
    f.close();
    
    // Validate key format (should be 32 hex characters)
    if (key.length() != 32) {
        Serial.printf("[CONFIG] Invalid WPA-SEC key length: %d (expected 32)\n", key.length());
        return false;
    }
    
    // Check all chars are hex
    for (int i = 0; i < 32; i++) {
        char c = key.charAt(i);
        if (!isxdigit(c)) {
            Serial.printf("[CONFIG] Invalid hex char in WPA-SEC key at position %d\n", i);
            return false;
        }
    }
    
    // Store the key
    wifiConfig.wpaSecKey = key;
    save();
    
    // Delete the file for security
    if (SD.remove(keyFile)) {
        Serial.println("[CONFIG] Deleted wpasec_key.txt after import");
        SDLog::log("CFG", "WPA-SEC key imported from file");
    } else {
        Serial.println("[CONFIG] Warning: Could not delete wpasec_key.txt");
    }
    
    return true;
}

bool Config::loadWigleKeyFromFile() {
    const char* keyFile = "/wigle_key.txt";
    
    if (!sdAvailable || !SD.exists(keyFile)) {
        return false;
    }
    
    File f = SD.open(keyFile, FILE_READ);
    if (!f) {
        Serial.println("[CONFIG] Failed to open wigle_key.txt");
        return false;
    }
    
    // Read the key file (format: apiname:apitoken)
    String content = f.readStringUntil('\n');
    content.trim();
    f.close();
    
    // Find the colon separator
    int colonPos = content.indexOf(':');
    if (colonPos < 1) {
        Serial.println("[CONFIG] Invalid WiGLE key format (expected name:token)");
        return false;
    }
    
    String apiName = content.substring(0, colonPos);
    String apiToken = content.substring(colonPos + 1);
    
    apiName.trim();
    apiToken.trim();
    
    if (apiName.isEmpty() || apiToken.isEmpty()) {
        Serial.println("[CONFIG] WiGLE API name or token is empty");
        return false;
    }
    
    // Store the keys
    wifiConfig.wigleApiName = apiName;
    wifiConfig.wigleApiToken = apiToken;
    save();
    
    // Delete the file for security
    if (SD.remove(keyFile)) {
        Serial.println("[CONFIG] Deleted wigle_key.txt after import");
        SDLog::log("CFG", "WiGLE API keys imported from file");
    } else {
        Serial.println("[CONFIG] Warning: Could not delete wigle_key.txt");
    }
    
    return true;
}
