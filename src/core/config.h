// Configuration management
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SPI.h>

#define CONFIG_FILE "/porkchop.conf"
#define PERSONALITY_FILE "/personality.json"

// GPS module source selection
enum class GPSSource : uint8_t {
    GROVE = 0,      // Grove port GPS (G1/G2) - works on all Cardputer models
    CAP_LORA = 1,   // Cap LoRa868 GPS (G15/G13) - Cardputer ADV only
    CUSTOM = 2      // Custom pins - user-configured
};

static constexpr uint8_t GPS_SOURCE_COUNT = 3;

// CapLoRa868 module pins (M5Stack Cardputer ADV EXT header)
// LoRa SPI shares MOSI/MISO/SCK with SD card - only CS differs
namespace CapLoraPins {
    static constexpr uint8_t LORA_CS    = 5;   // SX1262 NSS (chip select)
    static constexpr uint8_t LORA_RESET = 3;   // SX1262 NRESET
    static constexpr uint8_t LORA_DIO1  = 4;   // SX1262 DIO1 (interrupt)
    static constexpr uint8_t LORA_BUSY  = 6;   // SX1262 BUSY
    static constexpr uint8_t GPS_RX     = 15;  // GPS UART RX
    static constexpr uint8_t GPS_TX     = 13;  // GPS UART TX (default FSPIQ IOMUX!)
}

// GPS power management settings
struct GPSConfig {
    bool enabled = true;
    GPSSource source = GPSSource::GROVE;  // GPS module source (auto-selects pins)
    uint8_t rxPin = 1;              // G1 for Grove GPS, G15 for Cap LoRa868 (auto-set from source)
    uint8_t txPin = 2;              // G2 for Grove GPS, G13 for Cap LoRa868 (auto-set from source)
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

// G0 button action
enum class G0Action : uint8_t {
    SCREEN_TOGGLE = 0,
    OINK,
    DNOHAM,
    SPECTRUM,
    PIGSYNC,
    IDLE
};

static constexpr uint8_t G0_ACTION_COUNT = 6;

// Boot mode selection
enum class BootMode : uint8_t {
    IDLE = 0,
    OINK,
    DNOHAM,
    WARHOG
};

static constexpr uint8_t BOOT_MODE_COUNT = 4;

// ML settings
struct MLConfig {
    bool enabled = true;
    MLCollectionMode collectionMode = MLCollectionMode::ENHANCED;  // Data collection mode
    char modelPath[64] = "/m5porkchop/models/porkchop_model.bin";
    float confidenceThreshold = 0.7f;
    float rogueApThreshold = 0.8f;
    float vulnScorerThreshold = 0.6f;
    bool autoUpdate = false;
    char updateUrl[128] = "";
};

// WiFi settings for scanning and OTA
struct WiFiConfig {
    uint16_t channelHopInterval = 150;
    uint16_t spectrumHopInterval = 150;  // Spectrum-only sweep speed (ms)
    uint16_t lockTime = 12000;          // Time to discover clients before attacking (12s optimal, buffed 13s)
    bool enableDeauth = true;
    bool randomizeMAC = true;           // Randomize MAC on mode start for stealth
    int8_t spectrumMinRssi = -95;       // Spectrum: minimum RSSI to render (dBm)
    int8_t attackMinRssi = -70;          // OINK/DNH: ignore networks weaker than this (dBm)
    uint8_t spectrumTopN = 0;           // Spectrum: cap visible APs (0 = no cap)
    uint16_t spectrumStaleMs = 5000;    // Spectrum: stale timeout before drop (ms)
    bool spectrumCollapseSsid = false;  // Spectrum: merge same-SSID APs
    bool spectrumTiltEnabled = true;    // Spectrum: enable tilt-to-tune
    char otaSSID[33];
    char otaPassword[65];
    bool autoConnect = false;
    char wpaSecKey[33];                 // WPA-SEC.stanev.org user key (32 hex chars)
    char wigleApiName[65];              // WiGLE API Name (from wigle.net/account)
    char wigleApiToken[65];             // WiGLE API Token (from wigle.net/account)
};

// BLE settings for PIGGY BLUES mode
struct BLEConfig {
    uint16_t burstInterval = 200;       // ms between advertisement bursts (50-500)
    uint16_t advDuration = 100;         // ms per advertisement (50-200)
};

// Personality
struct PersonalityConfig {
    char name[32] = "Porkchop";
    char callsign[16] = "";             // User handle (unlocked at L10)
    int mood = 50;                      // -100 to 100
    uint32_t experience = 0;
    float curiosity = 0.7f;
    float aggression = 0.3f;
    float patience = 0.5f;
    uint8_t soundLevel = 1;    // 0=off, 1-5 volume steps
    uint8_t brightness = 80;            // Display brightness 0-100%
    uint8_t dimLevel = 20;              // Dimmed brightness 0-100% (0 = off)
    uint16_t dimTimeout = 30;           // Seconds before dimming (0 = never)
    uint8_t themeIndex = 0;             // Color theme (0-14, see THEMES array)
    G0Action g0Action = G0Action::SCREEN_TOGGLE;
    BootMode bootMode = BootMode::IDLE;
};

// JanusHog coprocessor settings (JANUS HOG)
struct C5Config {
    bool enabled = false;           // Opt-in: disabled by default
    uint32_t baudRate = 115200;     // UART baud rate
    uint16_t scanIntervalMs = 30000; // Auto-scan interval (0 = manual only)
    uint8_t uartTxPin = 2;         // Grove G2 default
    uint8_t uartRxPin = 1;         // Grove G1 default
};

class Config {
public:
    static bool init();
    static bool save();
    static bool load();
    static bool loadPersonality();
    static bool isSDAvailable();
    static bool reinitSD();  // Try to (re)initialize SD card at runtime
    static bool loadWpaSecKeyFromFile();  // Load key from /m5porkchop/wpa-sec/wpasec_key.txt (legacy /wpasec_key.txt)
    static bool loadWigleKeyFromFile();   // Load keys from /m5porkchop/wigle/wigle_key.txt (legacy /wigle_key.txt)
    static void prepareSDBus();           // Prepare SPI bus for raw SD access
    static void prepareCapLoraGpio();     // Quiesce SX1262 and clear G13 IOMUX before GPS UART
    static SPIClass& sdSpi();             // Access SD SPI bus
    static int sdCsPin();                 // Access SD chip-select pin
    
    // Getters
    static GPSConfig& gps() { return gpsConfig; }
    static MLConfig& ml() { return mlConfig; }
    static WiFiConfig& wifi() { return wifiConfig; }
    static BLEConfig& ble() { return bleConfig; }
    static PersonalityConfig& personality() { return personalityConfig; }
    static C5Config& c5() { return c5Config; }
    
    // Setters with auto-save
    static void setGPS(const GPSConfig& cfg);
    static void setML(const MLConfig& cfg);
    static void setWiFi(const WiFiConfig& cfg);
    static void setBLE(const BLEConfig& cfg);
    static void setPersonality(const PersonalityConfig& cfg);
    static void setC5(const C5Config& cfg);
    
private:
    static GPSConfig gpsConfig;
    static MLConfig mlConfig;
    static WiFiConfig wifiConfig;
    static BLEConfig bleConfig;
    static PersonalityConfig personalityConfig;
    static C5Config c5Config;
    static bool initialized;
    
    static bool createDefaultConfig();
    static bool createDefaultPersonality();
    static void savePersonalityToSPIFFS();
    static bool loadFrom(fs::FS& fs, const char* path);   // JSON migration only
    static bool applyJson(const JsonDocument& doc);        // JSON migration only
    static bool importCredsFromJsonConf();                 // Merge creds from porkchop.conf if present
};
